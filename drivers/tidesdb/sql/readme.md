# TidesDB SQL 入口

TidesDB 驱动将 `orm_raw()` 的受限 SQL 转换为已有 `orm_query_plan`，再执行原来的 TidesDB 后端。插件声明 `ORM_DRIVER_CAP_RAW_SQL`，但只接受下列方言，不提供 MySQL/PostgreSQL 全量兼容。

支持的语句：

```sql
SELECT id, score FROM people WHERE score >= ? AND note IS NOT NULL LIMIT ? OFFSET ?;
```

```sql
INSERT INTO people (id, score, note) VALUES (?, ?, 'O''Brien');
```

```sql
UPDATE people SET score = ? WHERE id = ?;
```

```sql
DELETE FROM people WHERE id = ?;
```

每次 `orm_raw()` 只提交一条语句，末尾分号可省略。通过 `orm_query_bind()` 从左到右绑定匿名 `?`，缺少或多余参数均报错。字符串和二进制参数作为值复制，不拼接进 SQL。

SELECT 使用 `orm_query_open_flow()`，写入使用 `orm_query_open_command_flow()`；显式事务使用对应的 `_in_transaction()` API。命令保持惰性执行，解析或执行失败通过 Publisher 的错误终态报告。完整、可编译的调用例子见 [SQL 集成测试](../../../orm/tests/integration/tidesdb/sql/sql_test.c)。

## 方言与边界

- 关键字不区分大小写；标识符保留大小写，只接受 `[A-Za-z_][A-Za-z_0-9]*`，最长 63 字节，不支持引号、限定名或别名。
- SELECT 必须显式列出唯一的列名；WHERE 支持 AND、`= != <> < <= > >=`、`IS NULL`、`IS NOT NULL`。
- 普通比较的右侧为 NULL（包括绑定 NULL）会明确报不支持，避免沿用结构化查询的 NULL 相等规则而误解 SQL。请使用 `IS NULL` 或 `IS NOT NULL`。
- INSERT 只支持单行 VALUES；必须提供后端配置的主键列。UPDATE/DELETE 沿用后端的主键等值定位约束：WHERE 必须包含主键，其他谓词也必须是非 NULL 等值比较；不执行全表批量修改。
- 字面量支持有符号/无符号 64 位范围内的十进制整数、单引号文本（`''` 转义）、TRUE/FALSE/NULL。浮点数、二进制和其他文本通过参数绑定；浮点参数必须有限。不做字符串与数字之间的隐式类型转换。
- 分页支持 `LIMIT 非负整数 [OFFSET 非负整数]`，也支持绑定整数；不支持独立 OFFSET。没有 ORDER BY 时，不保证业务意义上的行顺序。
- 不支持注释、`SELECT *`、JOIN、OR、括号表达式、函数、算术、子查询、聚合、排序、DDL、多语句、RETURNING、显式 SQL BEGIN/COMMIT；事务通过现有 ORM API 控制。异步查询仍不支持。

## 实现选择与兼容性

当前安装的 Salts/SaltsUtils 没有 SQL 解析入口。已有 `orm_sql_render.c` 是计划到 SQL 的反向转换；SQLite 的公开 prepare API 也不能提供独立 AST。因此复用仓库已使用的 re2c 生成词法分析器，使用无递归的语句解析器直接构造受限计划。没有引入外部解析库。

候选的 TideSQL/MariaDB 路线会引入服务器部署；完整 SQL 引擎则还需要表达式执行和优化器。本实现选择较小方言以保持嵌入式架构，并复用现有事务、行编码、条件匹配和扫描限额。

**MED｜兼容性边界：** 只扩展 TidesDB 对 raw SQL 的接受范围，不修改公开函数、插件 ABI、结构化查询语义、键编码或持久化格式。SQL 调用者必须遵守上述方言。更新驱动后即可使用；回滚到旧驱动后 SQL 再次返回不支持，原有结构化访问仍可使用同一数据。

## 所有权、限额与错误

原始 SQL 和绑定参数由 ORM 查询持有，是转换的事实源。解析为单线程同步过程，不引入线程、队列或全局可变状态；临时 token 只借用原始 SQL，结果计划深拷贝标识符和参数。命令的临时计划在返回后释放，SELECT 的计划由游标独占，直到取消后的销毁或正常销毁。游标保留原有连接及插件租约。

完整解析和参数核对发生在事务写入之前，不能执行多语句前缀。失败销毁部分计划；解析失败没有数据库副作用，执行阶段继续采用后端原有事务提交/回滚规则。所有权转移后不会再修改计划。

`max_query_bytes`、`max_parameters`、`max_columns`、`max_assignments`、`max_predicates` 和 `max_parameter_bytes` 约束输入与转换结果，字面量也计入参数字节预算。SELECT 继续受 `max_result_rows`、`max_result_bytes`、`max_scan_rows`、`max_scan_bytes` 约束；需求驱动逐行读取，不预先物化整表。

若 SQL 长度为 B、列数为 C、赋值数为 A，词法和语法扫描为 O(B)，重复列/赋值检查为 O(C²+A²)，标识符长度上限为 63；这些数量由上述限额控制。空间由计划中的列、赋值、谓词与参数字节决定，每次解析最多额外保留一个解码后的文本字面量。

语法错误为 `ORM_STATUS_SQL_ERROR`，不支持的语法为 `ORM_STATUS_UNSUPPORTED`，参数不匹配为 `ORM_STATUS_INVALID_ARGUMENT`，预算或整数范围超限为 `ORM_STATUS_LIMIT_EXCEEDED`，分配失败为 `ORM_STATUS_OUT_OF_MEMORY`。解析诊断包含 SQL 字节偏移，不打印 SQL 内容或参数。

## 验证

解析器用例位于 [TinyTest/TinyMock 测试](../../../orm/tests/flow/tidesdb/sql/parser_test.c)，覆盖绑定与复制、语法拒绝、截断输入、整数边界、NULL、限额和分配失败。实际 TidesDB 集成用例覆盖 CRUD、与结构化查询互通、事务和保存点、拒绝后无写入、游标生命周期及取消。

Windows 在 MSVC 开发环境中运行，依赖根沿用已有 preset：

```powershell
cmake --build --preset ci-windows-release --target orm_tidesdb_sql_parser_test orm_tidesdb_sql_test
ctest --preset ci-windows-release -R '^orm_tidesdb_'
```

Linux 使用 `ci-linux-release` 的同名 targets 和测试过滤。常规 CI 自动执行这些 CTest 用例，不增加独立 workflow。
