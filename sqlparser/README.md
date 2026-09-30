# sqlparser

独立 C11 SQL 解析模块，显式支持 MySQL / SQLite 两种方言的常用语法子集。
使用 re2c 分词、仓库自带 Lemon 生成 LALR 解析器，输出只读 AST。
CMake target 为 `sqlparser`，构建和安装后的别名均为
`TurboDB::SqlParser`，公开头文件为 `<sqlparser/sqlparser.h>`。

## 语法范围

默认入口采用 MySQL 常用语法子集；显式入口可以选择 SQLite。
两种模式都支持多语句和可省略的末尾分号。
空输入、空语句、仅普通注释的输入返回空文档。

### 官方依据与支持状态

MySQL 语法参考固定为 [MySQL 8.4 Reference Manual](https://dev.mysql.com/doc/refman/8.4/en/sql-statements.html)。
SQLite 以 [SQL 语法文档](https://www.sqlite.org/lang.html)核查规则，
以 3.50.4 的语法源和测试依赖固定当前实现基线；官网后续增加的语法不会自动成为本模块承诺。
下面记录解析器的实现状态，不代表数据库执行能力或完整标准符合性。

| 对照项 | MySQL 模式 | SQLite 模式 | 官方依据 |
| --- | --- | --- | --- |
| CTE 与嵌套查询 | 支持普通/递归 CTE、WITH SELECT / UPDATE / DELETE，以及 INSERT / REPLACE 的查询部分携带 WITH | 支持查询 CTE 和 WITH 前缀写入；MATERIALIZED 提示未实现 | [MySQL WITH](https://dev.mysql.com/doc/refman/8.4/en/with.html)、[SQLite WITH](https://www.sqlite.org/lang_with.html) |
| 保存点 | 支持创建、回滚和释放；RELEASE 必须含 SAVEPOINT | 支持创建、回滚和释放；RELEASE 可省略 SAVEPOINT | [MySQL 保存点](https://dev.mysql.com/doc/refman/8.4/en/savepoint.html)、[SQLite 保存点](https://www.sqlite.org/lang_savepoint.html) |
| 冲突后更新 | ON DUPLICATE KEY UPDATE 未实现 | ON CONFLICT … DO UPDATE / NOTHING 未实现；现有 INSERT OR 策略不是 UPSERT | [MySQL 冲突更新](https://dev.mysql.com/doc/refman/8.4/en/insert-on-duplicate.html)、[SQLite UPSERT](https://sqlite.org/lang_upsert.html) |
| 写入返回行 | 不声明支持 RETURNING | RETURNING 子句未实现 | [SQLite RETURNING](https://www.sqlite.org/lang_returning.html) |
| 窗口查询 | 支持基础 OVER（分区、排序及命名引用）；WINDOW 声明和窗口帧未实现 | OVER / WINDOW 子句未实现 | [MySQL 窗口函数](https://dev.mysql.com/doc/refman/8.4/en/window-functions.html)、[SQLite 窗口函数](https://www.sqlite.org/windowfunctions.html) |
| DELETE 排序及条数限制 | 支持单表 ORDER BY / LIMIT | 可通过构建开关启用，默认关闭；触发器内禁止 | [MySQL DELETE](https://dev.mysql.com/doc/refman/8.4/en/delete.html)、[SQLite DELETE](https://sqlite.org/lang_delete.html) |

证据分为三层：官网定义合法写法；grammar / lexer 决定本模块接受范围；
正式测试检查正反例、AST 内容和资源边界。SQLite 部分用例另做 3.50.4 引擎执行对照；
MySQL 另有固定 8.4.0 官方来源的 [mysql-spec 语料](mysql-spec/README.md)，
逐条记录上游预期和本地缺口，尚未做 MySQL 服务端执行对照。
语料文件接受数量单独报告，不作为完整兼容率。新增支持须同时更新规则、AST 契约、
正反例和下方范围表；未覆盖的官网语法保持明确记录。

### MySQL 模式

| 类别 | 支持内容 |
| --- | --- |
| 查询 | SELECT、DISTINCT、别名、限定名、星号、函数、子查询、WITH [RECURSIVE] CTE、UNION / UNION ALL、括号查询及分层 ORDER BY / LIMIT、JOIN / INNER / LEFT / RIGHT / CROSS / NATURAL JOIN、ON / USING、WHERE、GROUP BY、HAVING、ORDER BY、LIMIT / OFFSET；SQL_CALC_FOUND_ROWS；基础 OVER 窗口调用 |
| 表达式 | 数字、字符串、NULL、布尔值、`?`、`@变量` / `@@变量`、算术、比较、`<=>`、位运算、NOT / AND / XOR / OR、用户变量 := 赋值、BETWEEN、IN、LIKE / ESCAPE、IS NULL、EXISTS、CASE、CAST、COLLATE、DEFAULT(列名) |
| 写入 | INSERT / REPLACE 多行 VALUES、空行、INSERT SET、INSERT SELECT；VALUES 和 INSERT / REPLACE / UPDATE 赋值中的 DEFAULT；INSERT / REPLACE 查询部分的 WITH；WITH 前缀 UPDATE / DELETE；UPDATE / DELETE 的 WHERE、ORDER BY 和 LIMIT；LOW_PRIORITY；两种多表 DELETE 形式（不允许 ORDER BY / LIMIT） |
| 事务 | BEGIN [WORK]；START TRANSACTION 的 READ ONLY / WRITE、WITH CONSISTENT SNAPSHOT；COMMIT / ROLLBACK 的 AND [NO] CHAIN、[NO] RELEASE；SET TRANSACTION 隔离级别及读写模式；SAVEPOINT、ROLLBACK [WORK] TO [SAVEPOINT]、RELEASE SAVEPOINT；LOCK / UNLOCK TABLE[S] |
| 设置与查询 | SET 赋值及 GLOBAL / SESSION / LOCAL 作用域、系统变量的 DEFAULT；SET NAMES / SET CHARACTER SET（含 DEFAULT）；SHOW DATABASES / TABLES / TABLE STATUS / VARIABLES / COLLATION / TRIGGERS / EVENTS / OPEN TABLES / COLUMNS / INDEX / STATUS / CHARACTER SET / PROCEDURE STATUS / FUNCTION STATUS / CREATE TABLE，及各形式适用的修饰符、数据库、作用域和过滤条件 |
| 基础 DDL | CREATE [TEMPORARY] TABLE [IF NOT EXISTS]、列类型和参数、UNSIGNED / ZEROFILL、ON UPDATE 时间戳、列约束、主键 / 唯一键 / 索引 / 外键 / CHECK、ENGINE / CHARSET / COLLATE 选项；CREATE TABLE … [AS] SELECT；DROP [TEMPORARY] TABLE [IF EXISTS]；CREATE TABLE LIKE、TRUNCATE TABLE、ALTER TABLE ENGINE / ALTER COLUMN SET或DROP DEFAULT；CREATE VIEW 的 WITH [LOCAL或CASCADED] CHECK OPTION、DROP VIEW |
| 计划与维护 | EXPLAIN / DESCRIBE / DESC（含 FORMAT=TRADITIONAL / JSON / TREE）包装已支持的 SELECT、INSERT、REPLACE、UPDATE、DELETE（含合法位置的 CTE）；单表 ANALYZE TABLE |
| 预处理语句 | PREPARE 名称 FROM 字符串或用户变量；EXECUTE 名称 [USING 用户变量列表]；DEALLOCATE / DROP PREPARE 名称 |

关键字不区分大小写。反引号用于标识符，单双引号用于字符串，支持重复引号
及字符串中的反斜杠转义。`||` 按 OR 解析；本模块没有可变 `sql_mode`，
不支持 `ANSI_QUOTES`、`NO_BACKSLASH_ESCAPES`、`PIPES_AS_CONCAT`、`IGNORE_SPACE`。
这些模式会改变词法或运算符含义，详见 [MySQL SQL Modes](https://dev.mysql.com/doc/refman/8.4/en/sql-mode.html)。
表达式优先级采用 [MySQL 文档的运算符层级](https://dev.mysql.com/doc/refman/8.4/en/operator-precedence.html)，
比较高于 BETWEEN，显式 JOIN 高于逗号分隔的表引用。
标识符允许非 ASCII 字节；输入编码及字符集有效性由调用方保证。

按 [MySQL 名称规则](https://dev.mysql.com/doc/refman/8.4/en/identifiers.html)，
支持 `1ea10`、`1a20`、`1e` 等数字开头的名称，裸纯数字仍按数字字面量处理。
完整指数形式优先识别为数字，例如 `1e+10`；`1e10tail` 是数字及隐式别名。
限定名称的点号后按名称识别，包括 `t.1a20`、`t.1e10` 和 `t.select`，不会拆成小数。
这些规则不改变 SQLite 对数字后紧接名称的拒绝行为。

支持 [十六进制](https://dev.mysql.com/doc/refman/8.4/en/hexadecimal-literals.html)
`0x…` / `X'…'` 和 [二进制](https://dev.mysql.com/doc/refman/8.4/en/bit-value-literals.html)
`0b…` / `B'…'` 字面量，统一保存为 BLOB 原文节点；解析阶段不解码、不执行数值转换。
引号形式的 X/B 不区分大小写，0x/0b 前缀区分大小写；X 引号内容要求偶数个十六进制数字，
B 引号内容只允许 0/1。`0xG`、`0b102`、`0XFF` 等按名称识别，不伪装成字面量。
SQLite 的 `0xFF` 仍为 NUMBER，`X'00ff'` 仍为 BLOB。

按照 [MySQL 函数名解析规则](https://dev.mysql.com/doc/refman/8.4/en/function-resolution.html)，
COUNT、BIT_AND、CAST、SUM 等特殊函数名紧接 `(` 时识别为函数 token，
不能在该位置充当未限定、未引用的表名；空格、普通注释、反引号或数据库限定名
允许其作表名。ADDDATE、SUBDATE、SESSION_USER、SYSTEM_USER 保留官方测试中的
非保留名称行为。函数调用仍只支持现有表达式参数，不承诺完整内置函数专用语法或参数校验。
`SQL_CALC_FOUND_ROWS` 保存为 SELECT 的 `calc_found_rows` 标志，不会成为列名或别名；
支持与 DISTINCT / ALL 组合，拒绝同时指定 DISTINCT 与 ALL。反引号和限定名中的
同名标识符仍可使用。此修饰符的作用域、计数和服务端弃用策略不在解析层执行。

依据 [INSERT](https://dev.mysql.com/doc/refman/8.4/en/insert.html) 和
[SET 变量](https://dev.mysql.com/doc/refman/8.4/en/set-variable.html) 的规则，裸 `DEFAULT`
只作为写入值或系统变量的完整赋值值，不接受 `SELECT DEFAULT`、`DEFAULT+1`、
`SET @用户变量=DEFAULT`。`SET NAMES DEFAULT` 与 `SET CHARACTER SET DEFAULT` 分别依据
[SET NAMES](https://dev.mysql.com/doc/refman/8.4/en/set-names.html) 和
[SET CHARACTER SET](https://dev.mysql.com/doc/refman/8.4/en/set-character-set.html)。
[`DEFAULT(列名)`](https://dev.mysql.com/doc/refman/8.4/en/miscellaneous-functions.html#function_default)
是表达式，参数限于单个列名（可限定、可引用），允许参与运算；不接受任意表达式或参数列表。
列是否有可用的默认值、系统变量的类型/作用域与实际默认值由执行层检查。

CTE 复用双方言的 WITH / CTE AST；同一查询层只能有一个 WITH，多个绑定用逗号分隔，
子查询可嵌套 WITH。MySQL INSERT 的 WITH 位于查询部分，不接受 SQLite 的前置 WITH INSERT。
CTE 查询体目前限于已支持的 SELECT / UNION 子集；不校验递归引用、名称作用域或列数匹配。

支持 [MySQL 括号查询表达式](https://dev.mysql.com/doc/refman/8.4/en/parenthesized-query-expressions.html)，
包括嵌套括号、UNION 操作数、CTE、派生表、INSERT / REPLACE 查询部分、建表查询和 EXPLAIN。
例如 `(SELECT a FROM t LIMIT 7) ORDER BY a LIMIT 4` 保留两层查询尾部，
内层 LIMIT 不会被外层覆盖；UNION 的全局尾部也独立于各操作数。
标量子查询、普通表达式括号及 IN 表达式列表保持各自含义；`IN ((SELECT …))` 表示子查询。
这里只扩展已支持的 SELECT / UNION 查询体，TABLE、VALUES ROW、INTO 等其他形式仍未实现。

[`CREATE TABLE … [AS] SELECT`](https://dev.mysql.com/doc/refman/8.4/en/create-table-select.html)
可带已有的列定义和表选项，也可省略列定义；选项必须位于查询之前。
AST 的 `create_table.elements/options/query` 分别保存列定义、选项和查询，不合并列或推导类型。
查询体限于已支持的 SELECT / UNION / WITH；TABLE、VALUES ROW、IGNORE / REPLACE 前缀未实现。

[`EXPLAIN`](https://dev.mysql.com/doc/refman/8.4/en/explain.html) 的三个关键字同义形式
保存为 EXPLAIN 节点，`explain.statement` 指向被解释语句，MySQL 中 `query_plan` 为 false。
不接受任意 DDL / SET / 事务作为被解释语句；支持 FORMAT=TRADITIONAL / JSON / TREE，
ANALYZE、FOR CONNECTION、
表结构描述形式和其他选项未实现。[`ANALYZE TABLE`](https://dev.mysql.com/doc/refman/8.4/en/analyze-table.html)
目前仅接受单表，`maintenance.target` 保存名称；多表、LOCAL / NO_WRITE_TO_BINLOG 和直方图选项未实现。

[`CREATE VIEW`](https://dev.mysql.com/doc/refman/8.4/en/create-view.html) 支持可选的非空列名列表，
`create_view.name/columns/query` 保存名称、列名与查询；不支持 TEMPORARY / IF NOT EXISTS。
[`DROP VIEW`](https://dev.mysql.com/doc/refman/8.4/en/drop-view.html) 支持单个名称和 IF EXISTS。
OR REPLACE、ALGORITHM、DEFINER、SQL SECURITY、多视图删除等扩展未实现。

这不是 MySQL 服务端的完整语法或语义校验器。MySQL 模式的窗口帧、WINDOW 声明、存储程序、其他 ALTER 动作、
高级执行计划选项、完整外键动作、其他表选项等未列出的扩展返回语法错误。
版本注释 `/*! ... */` 和优化器提示 `/*+ ... */` 明确拒绝，避免静默忽略其语义。
普通 `/* ... */`、`# ...`、后接空白的 `-- ...` 注释被跳过。
不访问数据库，不解析绑定参数的值，不校验表是否存在、列类型、聚合合法性或权限。

依据 [SHOW](https://dev.mysql.com/doc/refman/8.4/en/show.html)，COLUMNS / FIELDS 可带
EXTENDED、FULL、FROM / IN 和 LIKE / WHERE；INDEX / INDEXES / KEYS 可带 EXTENDED 和 WHERE，
不能带 FULL 或 LIKE。SHOW CREATE TABLE 只保存目标表，不接受过滤条件。

[事务结束选项](https://dev.mysql.com/doc/refman/8.4/en/commit.html)中的 AND CHAIN 与 RELEASE
不能同时启用。[SET TRANSACTION](https://dev.mysql.com/doc/refman/8.4/en/set-transaction.html)
保留作用域、四种隔离级别和读写模式；允许两种特征以任一顺序出现，不允许重复特征。
[表锁](https://dev.mysql.com/doc/refman/8.4/en/lock-tables.html)保留每个目标的别名及 READ、
READ LOCAL、WRITE 模式；MySQL 8.4 不再接受旧的 LOW_PRIORITY WRITE 表锁模式。

[CREATE TABLE LIKE](https://dev.mysql.com/doc/refman/8.4/en/create-table-like.html)支持带括号和
不带括号的源表；[TRUNCATE](https://dev.mysql.com/doc/refman/8.4/en/truncate-table.html)保持独立
DDL 节点。[ALTER TABLE](https://dev.mysql.com/doc/refman/8.4/en/alter-table.html)目前只新增单个
ENGINE 或 ALTER [COLUMN] SET / DROP DEFAULT 动作，不支持多动作列表。
[ZEROFILL](https://dev.mysql.com/doc/refman/8.4/en/numeric-type-attributes.html)与显式 UNSIGNED
分别记录；其隐含 unsigned 语义由执行层推导。ON UPDATE 的
[当前时间戳](https://dev.mysql.com/doc/refman/8.4/en/timestamp-initialization.html)及官方同义形式
保存为 CALL，可带无符号整数精度；不校验列类型、精度范围或默认精度是否匹配。

[PREPARE](https://dev.mysql.com/doc/refman/8.4/en/prepare.html)的字符串或用户变量只按原文保存，
不递归解析动态 SQL，也不维护服务端会话。[EXECUTE USING](https://dev.mysql.com/doc/refman/8.4/en/execute.html)
只接收用户变量，拒绝系统变量和任意表达式；参数数量与已准备语句是否存在由执行层检查。
[DEALLOCATE / DROP PREPARE](https://dev.mysql.com/doc/refman/8.4/en/deallocate-prepare.html)保留语句名。

[表达式优先级](https://dev.mysql.com/doc/refman/8.4/en/operator-precedence.html)为 AND 高于 XOR
高于 OR，用户变量 := 最低且右结合；赋值表达式使用 ASSIGNMENT 节点。
[多表 DELETE](https://dev.mysql.com/doc/refman/8.4/en/delete.html)分开保存目标列表与 JOIN 来源，
拒绝 ORDER BY / LIMIT；LOW_PRIORITY 是独立标志，不改变 AST 中的查询和赋值结构。

[基础窗口调用](https://dev.mysql.com/doc/refman/8.4/en/window-functions-usage.html)支持内置窗口/
可窗口化聚合函数后的 OVER()、PARTITION BY / ORDER BY，以及 OVER 名称引用。
函数与窗口定义分别保留，窗口排序不覆盖 SELECT 排序；拒绝普通函数的 OVER。
WINDOW 声明、括号内窗口继承、ROWS / RANGE 帧、NULL 处理修饰符尚未实现，
不检查函数参数签名、引用窗口是否存在或窗口函数是否位于合法执行位置。
[视图 CHECK OPTION](https://dev.mysql.com/doc/refman/8.4/en/create-view.html)保存省略、
默认、LOCAL 和 CASCADED 四种状态，不验证视图可更新性。

### SQLite 模式

SQLite 规则以 [SQLite 3.50.4 的语法源](https://github.com/sqlite/sqlite/blob/version-3.50.4/src/parse.y)
和[官方 SQL 文档](https://www.sqlite.org/lang.html)为参考。该模式与 MySQL 分别生成解析表，
并非在 MySQL 解析失败后重试。支持范围如下：

| 类别 | 支持内容 |
| --- | --- |
| 查询 | 基础 SELECT、子查询、JOIN、NATURAL INNER / CROSS / 外连接、带 ON / USING 的逗号连接、括号表组、表值函数、INDEXED BY / NOT INDEXED、GROUP BY / HAVING、ORDER BY / LIMIT；UNION / UNION ALL / INTERSECT / EXCEPT；VALUES；WITH [RECURSIVE] CTE |
| 表达式 | 公共算术、比较、CASE、CAST、COLLATE；`==`、连接符 `\|\|`、IS / IS NOT / IS [NOT] DISTINCT FROM、ISNULL / NOTNULL / NOT NULL、GLOB / REGEXP / MATCH、空 IN 列表、IN 表名 / 表值函数、RAISE |
| 词法 | 双引号、反引号和方括号标识符；单引号字符串不做反斜杠转义；十六进制整数、数字分隔下划线、`X'00ff'` BLOB；`?` / `?NNN` / `:name` / `@name` / `$name` 参数，含 `$ns::name(suffix)` |
| 写入 | INSERT / REPLACE INTO VALUES / SELECT、INSERT OR ROLLBACK / ABORT / FAIL / IGNORE / REPLACE、DEFAULT VALUES、UPDATE OR 冲突策略、UPDATE / DELETE 的 INDEXED BY / NOT INDEXED；WITH 前缀的 INSERT / UPDATE / DELETE |
| 事务 | BEGIN [DEFERRED / IMMEDIATE / EXCLUSIVE] [TRANSACTION]、COMMIT / END / ROLLBACK [TRANSACTION]、SAVEPOINT、RELEASE、ROLLBACK TO |
| 建表 | CREATE [TEMP / TEMPORARY] TABLE、无类型列及多词/带引号类型名、CREATE TABLE AS 查询、命名列约束及表约束、表级 PRIMARY KEY / UNIQUE 的 ASC / DESC / COLLATE、REFERENCES 的 ON DELETE / ON UPDATE / MATCH 和 DEFERRABLE / INITIALLY、约束 ON CONFLICT、列级及表级主键 AUTOINCREMENT、列 COLLATE、WITHOUT ROWID / STRICT；DROP TABLE |
| 扩展 DDL | CREATE VIRTUAL TABLE；CREATE [TEMP] TRIGGER / DROP TRIGGER、BEFORE / AFTER / INSTEAD OF、UPDATE OF、WHEN 和语句体；ALTER TABLE 的 RENAME TO、RENAME COLUMN、ADD COLUMN、DROP COLUMN |
| 其他 | PRAGMA 查询及赋值、表达式/部分/唯一索引的 CREATE INDEX、DROP INDEX、CREATE / DROP VIEW、ATTACH / DETACH；EXPLAIN [QUERY PLAN]；ANALYZE、REINDEX、VACUUM [schema] [INTO expr] |

SQLite 的比较、位运算、连接和 ESCAPE 使用独立优先级；逗号与显式 JOIN 从左向右结合。
如 `SELECT 1+2||3*4` 中，SQLite 的连接先于乘法，MySQL 默认模式则把 `||` 作为 OR。
SQLite 在允许标识符的上下文中接收部分非保留关键字；这属于语法规则，不改变所选方言。
SQLite 的连接关键字可用于名称，GLOB / REGEXP 也可作函数名；单引号限定符支持
`'alias'.column`、`'alias'.'column'` 和 `'alias'.*`，独立单引号表达式仍是字符串。
COLLATE 名称可用单引号；DEFAULT 支持带正负号的字符串字面量，保留一元运算 AST。
SQLite 的 `--` 注释无需后接空白，块注释可以在 EOF 结束；不支持 MySQL 的 `#` 注释。
双引号始终表示标识符，不模拟 SQLite 历史上的双引号字符串兼容行为。

[虚拟表](https://www.sqlite.org/lang_createvtab.html)的 USING 参数由具体模块定义语义，
解析器保留各参数的原文并校验括号结构，不把它们强行解释为 SQL 表达式。
[触发器](https://www.sqlite.org/lang_createtrigger.html)体保留各条语句的 AST，拒绝限定名写入目标、
DEFAULT VALUES、UPDATE / DELETE 的索引提示、ORDER BY / LIMIT 和直接 WITH 前缀的写入语句；
WITH SELECT 和 INSERT 内部的 WITH 查询按 3.50.4 引擎对照测试接受。

按 [3.50.4 官方语法源](https://github.com/sqlite/sqlite/blob/version-3.50.4/src/parse.y)
支持重复及悬空的 `CONSTRAINT name` 声明、可省略逗号的连续表约束。
表约束必须位于列定义之后，第一项表约束前必须有逗号；不接受表约束之后再声明列。
表级 CHECK 的 ON CONFLICT 保留在 AST 中；SQLite 引擎接受但忽略此策略。

`SQLPARSER_SQLITE_ENABLE_UPDATE_DELETE_LIMIT` 构建选项默认 OFF，对应 SQLite 官方的
[`SQLITE_ENABLE_UPDATE_DELETE_LIMIT`](https://sqlite.org/compile.html#enable_update_delete_limit)。
开启后 SQLite 模式接受 [UPDATE](https://sqlite.org/lang_update.html) /
[DELETE](https://sqlite.org/lang_delete.html) 的 ORDER BY、LIMIT、OFFSET 和逗号形式；
ORDER BY 必须同时带 LIMIT。触发器体的 UPDATE / DELETE 始终禁止这些子句，
但其子查询内的 LIMIT 仍可使用。MySQL 模式不受此开关影响。
LIMIT / OFFSET 表达式按原文保留，不在解析器中求值或检查整数转换；负值的执行含义
及选行顺序由后续执行层负责，ORDER BY 不承诺实际修改行的先后顺序。

未实现的 SQLite 范围包括窗口函数、UPSERT / RETURNING、UPDATE FROM、生成列、
任意顺序的 JOIN 关键字组合，以及部分特殊关键字/单引号标识符上下文等。
本模块只产出语法 AST：不检查 STRICT 类型、AUTOINCREMENT 的列类型或主键合法性，
不分配绑定参数槽位，不检查 SQLite 连接的变量数量上限，也不验证函数是否存在。
未实现语法返回错误；解析成功不代表可以在任意 SQLite 构建或数据库 schema 中执行。

## 接口与 AST

`sqlparser_parse(sql, length, limits, &document, &error)` 保持 MySQL 默认模式。
`sqlparser_parse_dialect(sql, length, dialect, limits, &document, &error)` 显式选择
`SQLPARSER_MYSQL` 或 `SQLPARSER_SQLITE`，整个批次使用同一方言。
方言记录在结果文档中，通过 `sqlparser_get_dialect(document)` 读取；NULL 文档返回
`SQLPARSER_DIALECT_UNKNOWN`。未知方言参数返回 `SQLPARSER_INVALID_ARGUMENT`。
`sqlparser_sqlite_update_delete_limit_enabled()` 无参数，返回当前静态库是否启用了
有限行写入扩展，无失败状态、不持有资源；它不查询外部 SQLite 引擎的编译选项。

两个解析入口精确读取 `length` 字节，
输入无需 NUL 终止。`sql == NULL` 仅允许 `length == 0`；内嵌 NUL 返回语法错误。
调用前 `document` 必须为 NULL；已有文档会被拒绝且保留原值。
`limits == NULL` 使用默认值，`error == NULL` 则不输出诊断。

成功返回 `SQLPARSER_OK` 并交付文档；失败时没有部分 AST，输出仍为 NULL。
错误区分参数错误、语法错误、资源超限和内存不足。错误包含零起始字节偏移及
定长消息，EOF 的偏移等于输入长度。消息不分配内存，分配失败时仍能读取状态。

`sqlparser_statements()` 返回语句列表；从 `first` 开始，通过节点的 `next`
按输入顺序遍历。`sqlparser_get_node()` 根据 ID 返回 const 节点，ID 0 表示缺省。
节点的 `kind` 决定有效的 union 成员；例如 SELECT 使用 `as.select`，UNION
使用 `as.compound`，DELETE 使用 `as.delete_stmt`。UNION 后的排序和分页属于
整个 compound 节点。列表的 `next` 仅表示兄弟关系，表达式子节点使用专门字段。

MySQL 新增 `SQLPARSER_QUERY_GROUP`：`as.query_group.query` 指向括号内查询，
`order_by` 和 `limit` 只属于右括号之后这一层，`span` 覆盖括号及该层尾部。
该节点可作为顶层语句、集合操作数，或出现在其他节点的 query 字段中。
标量子查询仍由 SUBQUERY 表示，其内部也可能包含 QUERY_GROUP。

选择独立节点是为了避免扁平化括号时覆盖内层 ORDER BY / LIMIT，也避免生成 SQL 中
并不存在的 SELECT 或 UNION。存储仍归属文档的有界节点容器，每层增加一个节点，
失败时沿用整批无 AST 的错误契约；不执行查询合并，也不模拟 MySQL 优化后的 63 层限制，
实际资源上限由 `max_nodes` / `max_stack_entries` 决定。
枚举只在末尾增加新值，已有字段和节点含义保持不变；消费方的 AST visitor / switch
需要处理 QUERY_GROUP，特别是原先把多层括号当成单层标量子查询的调用方。
如果消费方尚未适配，应停留在扩展前版本；回退时恢复拒绝这些查询，不能丢弃分层子句。

SQLite 的 INTERSECT / EXCEPT 也使用 `SQLPARSER_UNION` 节点，以
`as.compound.kind` 区分集合运算；已有 UNION 的枚举值和字段保持原义。
CTE 使用 `SQLPARSER_WITH` 的 bindings/body 与 `SQLPARSER_CTE` 的 name/query/columns。
SQLite 的 INSERT VALUES 保留 `as.insert.query` 指向 `SQLPARSER_VALUES`，
本轮沿用 lexer → 双方言 Lemon → 文档内 AST 的分层，新增语法不引入执行器、会话状态或
外部依赖。相比把选项塞入原文/普通 NAME，显式枚举与字段让消费方能识别其含义；
代价是公开 AST 扩展，所有权、错误传播和容量预算仍由同一个 document 管理。

**MED：AST 兼容性。** 消费方应与库一起重新编译，并适配以下新增内容：

- SHOW 的 table / extended；TRANSACTION 的 chain / release / access / isolation / scope / consistent_snapshot。
  CHOICE_UNSPECIFIED 与 CHOICE_NO 不同；SET_TRANSACTION 使用 TRANSACTION 节点，默认 scope 表示下一事务。
- CREATE_TABLE 的 like_table；TYPE 的 zerofill；CONSTRAINT 的 ON_UPDATE；ALTER 的新动作及 value；
  TRUNCATE_TABLE 使用 maintenance.target。MySQL CURRENT_TIMESTAMP 及同义形式现在保存为 CALL。
- PREPARE / EXECUTE / DEALLOCATE 的 prepared.name / source / parameters；LOCK_TABLES 的目标链及
  LOCK_TARGET 的 table / alias / mode；UNLOCK_TABLES 无载荷。
- SELECT.calc_found_rows；CREATE_VIEW.check；EXPLAIN.format；WINDOW.call / name / partition_by / order_by。
- 写入的 low_priority；多表 DELETE 的 targets / from，目标为 NAME 或带限定名的 STAR，
  单表字段 table 在多表形式中为空。ASSIGNMENT 也可能作为表达式出现，XOR 是新的 BINARY 运算符。

既有枚举值保留；新节点和字段均不携带文档外借用指针。解析只生成状态描述，遇到语法、
节点预算或分配错误仍整体销毁文档。正式测试覆盖字段归属、正反例、截断、方言隔离及
分配失败；回退应恢复旧解析器和匹配头文件，明确拒绝新增语法，不能忽略新增字段继续执行。

MySQL 的 INSERT VALUES 继续使用 `as.insert.rows`。SQLite DEFAULT VALUES 用独立布尔字段表示。
MySQL 裸 `DEFAULT` 保存为新增的 `SQLPARSER_DEFAULT_VALUE` 原子节点，无 union 载荷；
`DEFAULT(列名)` 保存为 CALL，name 是 DEFAULT 原文，arguments 只有一个 NAME。
这样保持默认值关键字与 NULL、字符串和函数调用可区分，不在解析期求值。
节点沿用文档的有界存储与失败原子性；没有增加依赖或改变已有枚举值。
**MED：兼容性。** 消费 AST 的 visitor 需处理新增 DEFAULT_VALUE 节点；旧版消费方可继续使用
旧版解析器明确拒绝这些新语句，不能忽略节点后继续执行。正式测试验证节点、作用域、
非法组合、SQLite 隔离、截断输入、节点预算及分配失败清理。
新增语句分别使用 virtual_table、trigger、explain、maintenance、alter 成员。
EXPLAIN 包装 statement；触发器 steps 是独立语句链，不混入顶层列表；
ALTER 的 ADD COLUMN 用 column 指向列定义，其余动作用 column / new_name 表示名称。
虚拟表参数是 `SQLPARSER_MODULE_ARGUMENT` 原文节点；无括号时列表为空，
空括号保留一个空参数，与 SQLite 的模块参数分割方式一致。

`as.name.parts` 记录限定名部分数，带引号名称内部的点不增加部分数。
表引用的 arguments / table_function 保存表值函数，group 保存括号表组，
indexed_by / not_indexed 保存索引提示；UPDATE / DELETE 也有同名提示字段。
NATURAL 外连接保留实际连接类型及 natural 标志；IN 表引用保存在 `as.in.table`。
SQLite NATURAL CROSS JOIN 保留 CROSS 类型和 natural 标志；逗号连接的 ON / USING
保存在对应 JOIN 节点中，继续遵循从左向右的结合顺序。
SQLite 表级 PRIMARY KEY / UNIQUE 的 `as.constraint.key_terms` 保存 ORDER 节点，
其 expression 为列名或带 COLLATE 的列名；原有 columns 是从这些项派生的列名列表，
引用同一组 NAME 节点，保持原接口的列名语义。表级 AUTOINCREMENT 使用现有标志。
SQLite 的 `as.constraint.declarations` 保存紧邻该约束之前的 CONSTRAINT 名称链，
元素为 NAME；`as.constraint.name` 保存最后声明的活动名称，作用域到下一个声明、
表约束之间的分隔逗号或新列为止。引入第一项表约束的逗号按 SQLite 历史规则
不清除最后一列的活动名称；测试同时检查原生引擎的 CHECK 错误名称。
COLUMN_COLLATION 的 name 仍保存排序规则名称，
其约束名称从 declarations 读取。声明没有后续约束时，在原列表位置生成
`SQLPARSER_CONSTRAINT_DECLARATION`，保留名称链和完整源位置；它不是可执行约束。
普通命名约束仍直接位于 constraints / elements 列表中，不额外插入声明节点。
外键动作保存在 `as.constraint.reference`；列级 DEFERRABLE 是独立的
`SQLPARSER_DEFERRABILITY` 约束，表级外键则将延迟属性放在自身 reference 中。
缺省动作与显式 NO ACTION 可区分；重复同类动作按最后一次声明保留。
类型名原文包含所有词；例如 SQLite 把 `INT AUTO_INCREMENT` 作为类型名，
不会生成 MySQL 的 AUTO_INCREMENT 约束节点。

无类型列的 `as.column.type` 为零，SQLite 表选项使用 `SQLPARSER_TABLE_OPTION`，
MySQL 表选项继续使用赋值节点。CAST、COLLATE、PRAGMA、索引、视图、ATTACH / DETACH
有独立节点；事务模式、冲突策略和主键 AUTOINCREMENT 均保留在类型化字段中。

`sqlparser_node_count()` 返回文档存储的节点数，包括构造限定名时的中间节点。
`sqlparser_text(document, node->span)` 返回原始 SQL 切片：保留引号、转义和数字
格式，不在切片尾部添加 NUL。结构节点的 span 可能含尾部空白或注释，不用于
重新生成规范 SQL。所有节点和文本只借用文档，直到
`sqlparser_document_destroy()` 才失效。无效 ID / span 返回 NULL，NULL 文档的
列表和计数为空，destroy(NULL) 无操作。

完整示例见 [examples/parse.c](examples/parse.c)，由 `BUILD_EXAMPLES` 控制构建。
默认执行输出两条语句的类型和源位置，也可传入 SQL；可选前缀 `--mysql` / `--sqlite`
指定方言。例如 `sqlparser_example --sqlite "SELECT 'a'||'b'; PRAGMA cache_size"`。

## 架构与内存协议

选择独立的 lexer → Lemon grammar → AST 分层。现有 TidesDB 解析器直接降低到
执行计划，不能保存 JOIN、子查询和 DDL 的完整结构；直接搬入 `lemon-master`
又会带入 SQLite 内部类型和遗留分配状态。因此复用仓库代码生成工具，以用户提供的
lemon-master 为初始语法组织参考，重新实现公开 AST 和错误边界。
相比扩展 TidesDB 专用解析器，此方案多一个 AST 层，但没有数据库或 ORM 依赖。

- **数据与事实源：** 文档拥有一份 `tstr` SQL 副本和有上限的 CSTL `vec_t` 节点存储。
  节点关联使用编号，容器扩容不影响关联；解析期间不跨扩容保留节点指针。
- **状态迁移：** 校验参数及长度 → 复制输入 → 构建私有 AST → 整个批次接受后交付。
  lexer 错误、语法错误、OOM、超限均销毁整份私有文档，无外部副作用或回滚数据。
- **方言边界：** 一个 re2c 源按请求方言识别 token，一个 Lemon 源经 `SQLITE` 定义
  生成两套解析表，共享显式 token 编号和 AST 工具。相比复制两份完整语法，公共规则
  保持单一来源；相比一套优先级加运行时补丁，两套表能直接表达优先级和 JOIN 结合差异。
  代价是增加一份解析表和生成步骤，没有新增运行时依赖或共享可变状态。
- **所有权：** Lemon 栈只保存 span、ID、列表值，不单独释放 AST 子树。
  文档集中销毁容器和输入，避免深树递归析构和错误规约双重释放。
- **线程模型：** 每次调用 single-threaded、状态私有，无全局分配失败标志或互斥锁。
  独立调用可并发；文档在调用方安全发布后支持并发只读，销毁前必须结束所有读取。
- **容量：** 默认输入 1 MiB、节点 65536、语句 1024、栈 4096 项。
  四项通过 `sqlparser_limits` 调整；栈范围为 2–1048576，节点受 ID 和字节乘积上限限制。
  语句预算计入顶层语句和触发器体的每个步骤；一个含两步的 CREATE TRIGGER 消耗 3 条，
  顶层列表仍只有一个节点。EXPLAIN 包装和 CTE 查询不额外增加该计数。
  到限立即返回 `SQLPARSER_LIMIT_EXCEEDED`，不丢弃、不阻塞、不降级。
- **预算：** 节点容量不超过 `max_nodes * sizeof(sqlparser_node)`，另有输入副本、
  `tstr` / CSTL 元数据和 Lemon 栈；栈容量不超过 `max_stack_entries` 项。
  扩容期间需为分配器可能同时保留新旧存储预留空间。调用方的原输入不计入模块所有权。
- **复杂度：** 固定语法下，分词和语法规约为 O(输入字节数 + 节点数)，追加节点摊销 O(1)。
  存储为 O(输入长度 + 节点容量 + 栈容量)；释放不随 AST 深度使用递归调用栈。
- **观测：** 返回状态、错误字节位置、语句数和节点数。模块不记录 SQL 或日志。

**兼容性与迁移：** 新增可关闭的 `TURBODB_BUILD_SQLPARSER`（默认 ON）和独立静态库，
复用既有 re2c/Lemon、Salts::CSTL 与 Salts::Core，不增加第三方依赖。
现有 ORM、TidesDB 驱动和持久化格式不改变。后续接入执行器需要独立的 AST → plan
适配器及方言契约测试；关闭模块即可回退构建，新消费者需要移除该 target 链接。
本模块不是 `sqlite3RunParser` 的 ABI 替代品。

**双方言迁移：** 旧调用无需修改；SQLite 消费者使用新入口并检查文档方言。
公开 AST 新增枚举和字段，消费者必须与静态库一起重新编译，不承诺旧二进制布局兼容。
不迁移数据库或存储格式。回滚到 MySQL 行为时改回旧入口；回滚版本时同步回滚头文件与库。
验证范围包含两套语法生成、旧 API 回归、跨方言正反例、AST 差异、截断输入和分配失败。

**MED：MySQL 关键字兼容边界。** CTE / 保存点支持将 WITH、RECURSIVE、RELEASE、TO
按 [MySQL 8.4 保留字规则](https://dev.mysql.com/doc/refman/8.4/en/keywords.html)识别，
此前被当作普通名称接受的裸词需要改用反引号；这些词在限定名的点号后仍可使用。
非保留字 SAVEPOINT 仍可作普通标识符。此改动不增加公开字段或依赖，验证覆盖
关键字、带引号名称、限定名、WITH 的位置、空 CTE 列表及同层重复 WITH。

**MED：SQLite 表级键 AST 扩展。** 新增 key_terms 字段，调用方须与静态库同步重新编译。
原有 columns 仍为 NAME 列表；需要排序及排序规则的消费者应读取 key_terms。
验证覆盖列表关联、排序/排序规则、自增、双方言隔离、SQLite 引擎对照、截断和分配失败。

**MED：历史约束 AST 扩展。** 新增 declarations 字段和 CONSTRAINT_DECLARATION 枚举，
消费者须同步重新编译，并在遍历约束时区分悬空名称声明。活动名称状态只存在于
当前列/表约束的 Lemon 规约值中，不跨语句共享；名称节点归文档所有，受节点上限约束。
验证覆盖连续声明、名称覆盖/继承、逗号清除、列边界、COLLATE、ALTER ADD COLUMN、
SQLite 执行对照、逐节点配额和分配失败。关闭有限行写入开关可恢复其旧默认接受范围；
历史约束 AST 的版本回滚须同步回滚头文件和静态库。

re2c 的输入副本使用[官方 sentinel with bounds checks 协议](https://re2c.org/manual/manual_c.html#sentinel-with-bounds-checks)，
Lemon 使用仓库 [lemon.c](../tools/lemon/lemon.c) 和 [lempar.c](../tools/lemon/lempar.c)
提供的 extra_context、stack_size_limit、realloc/free 接口。

## 构建与验证

在 VS 开发者命令行中，或先通过 `VsDevCmd.bat -arch=x64 -host_arch=x64` 建立环境：

```text
cmake --preset win-release-user
cmake --build --preset win-release-user --target sqlparser_test sqlparser_failure_test sqlparser_dialect_test sqlparser_sqlite_extension_test sqlparser_corpus_test sqlparser_mysql_corpus_test sqlparser_benchmark sqlparser_example
ctest --preset win-release-user -R "^sqlparser_" --output-on-failure
```

验证可选有限行写入扩展时，配置命令改为：

```text
cmake --preset win-release-user -DSQLPARSER_SQLITE_ENABLE_UPDATE_DELETE_LIMIT=ON
```

随后运行同一组 build/test 命令。切回默认语法时显式传 `OFF` 并重建；CMake 缓存会
保留上次选择。构建/导出的 target 同时传播数值为 0 或 1 的同名宏，手动链接的消费者
可用上述运行时查询函数确认库能力。测试分别覆盖 ON / OFF；原生 SQLite 测试依赖的
编译开关独立判断，未启用该选项的引擎只做拒绝对照，不能作为扩展执行行为已验证的证据。

生成的 C 和 token 头只写入 build tree。安装复用 TurboDBTargets，消费者链接
`TurboDB::SqlParser`。开发环境依赖完整时可使用 `win-dev-user` 运行 ASan。

测试覆盖常用语句及 AST 内容、优先级、UNION 排序归属、带长度且无 NUL 的输入、
截断前缀、非法尾字符、原输入改写后的所有权、批次失败原子性、四项资源边界、
大型表达式，以及逐个分配边界失败后资源计数归零和新请求恢复。
Benchmark 分别测量普通查询和达到语句配额的批次，计时包含输入复制、解析及销毁；
字节数仅计算每份 SQL 输入一次，不代表数据库执行性能。
`sqlparser_dialect_test` 和 `sqlparser_sqlite_extension_test` 另外链接项目已有 SQLite3 测试依赖，在内存数据库中执行代表性
SQLite 批次并验证关键表达式结果；生产库不链接 SQLite3。

### SQLite 语料测试

SQLite 方言语料位于 `sqlparser/sqlite-spec`。目录中存在 `.sql` 文件时，
CMake 注册 `sqlparser_corpus_test`：

```text
cmake --build --preset win-release-user --target sqlparser_corpus_test
ctest --preset win-release-user -R "^sqlparser_corpus_test$" --output-on-failure
```

测试按原始字节和默认资源上限，将每个文件分别以两种方言作为完整批次解析，不改写 SQL。
MySQL 报告写入 `<build>/sqlparser/tests/corpus-results.tsv`，SQLite 报告写入同目录
`corpus-sqlite-results.tsv`。两份报告包含状态、字节数、
成功批次语句数、首个错误的零起始偏移、从 1 开始的行号与字节列号，以及错误上下文。
一个文件中任一语句失败即拒绝整份批次，因此文件接收率不代表语句接收率。

语料含 SQLite 官方测试和故意错误的输入，不是 MySQL 兼容性通过清单，
也不是每个文件都应成功的 SQLite 完整规范测试。MySQL 模式结果仅用于观察方言差异。
测试断言已知正反例、成功 AST 的节点切片和语句链、失败结果的原子性与错误位置；
其他文件的语法拒绝和资源超限计入报告，不作为测试程序失败。
异常状态、文件读取失败或上述结果契约破坏会使测试失败。
该测试不执行 SQL，也不证明 SQLite/MySQL 语义等价。

本轮 1028 个文件的结果（默认资源上限）：SQLite 开关 OFF 时接受 1020、语法拒绝 7、
资源超限 1；ON 时接受 1023、语法拒绝 4、资源超限 1。MySQL 均接受 168、拒绝 860。
两个历史约束文件固定断言接受，三个有限行写入文件按开关断言接受或拒绝。
ON 状态剩余语法拒绝分别是 `comments/basic-comments.sql`（嵌套块注释）、
`official-suite/substr-1.sql`（未展开的 `x'sub_hex'`）、`parse-errors/parse-error-1.sql`
（重复 WHERE）、`select/select-alt-syntax.sql`（VALUES 后接 ORDER BY）；
`official-suite/fuzzer1-1.sql` 超过默认 1024 条语句预算，保持明确资源错误。

### MySQL 官方语料测试

`sqlparser/mysql-spec` 保存固定 MySQL 8.4.0 来源和 385 条原文 SQL 片段，
由 `sqlparser_mysql_corpus_test` 使用显式 MySQL 方言逐条解析。原生语法错误、
执行错误与本地语法缺口分别记录；已知差异不会被包装成完整兼容。
本轮接受 298、拒绝 87，包含 77 条缺失语法与 10 条误接收；其中 10 条误接收是
**HIGH** 级语法合法性判断偏差，涉及内置函数名紧接左括号作表名。
来源、许可、分类契约和重建步骤见 [mysql-spec/README.md](mysql-spec/README.md)。
结果写入 `<build>/sqlparser/tests/mysql-spec-results.tsv`，不覆盖原有 SQLite 语料报告。
