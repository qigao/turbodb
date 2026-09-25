# Driver-specific DDL SQL tools

## 背景与目标

TurboDB 的 ORM query backend 面向单条参数化查询，不承担数据库初始化脚本执行职责。
SQLite query 路径会拒绝第二条 statement；PostgreSQL query 路径使用
`PQsendQueryParams()`，也不是多 statement DDL 文件的执行契约。

`dbtools` 只负责通过选定的 native driver 执行一个已有的、有界 DDL SQL 文件。SQL
可以由任何外部工具或人工生成；dbtools 不解析 schema 文件、不生成 SQL、不解释记录格式，
也不提供 JSON、CSV 或其他数据导入导出能力。

目标如下：

- 生产实现保持 C11；C++ 不进入工具 target 的 production sources。
- 提供独立的 `turbodb-sqlite` 和 `turbodb-postgresql` executable。
- 每个 executable 静态绑定一个 native driver，不使用运行时 registry 或 service locator。
- 直接链接 SQLite/libpq，不链接 `turbo_orm`、CFlow、Salts parser targets、DataBind 或 CSerde。
- 对文件、配置、事务和 native error 实施 fail-fast、有界、可复验的契约。

非目标包括 schema-to-SQL 编译、migration diff、`ALTER TABLE` 规划、migration history、
通用 SQL shell、DML 数据搬运、数据库间复制和运行时 driver plugin。

## 架构边界

```text
DDL SQL file
    |
    v
CLI + bounded file reader
    |
    v
static schema-driver ops
    |----------------------|
    v                      v
SQLite sqlite3_exec()      PostgreSQL PQsendQuery() + PQgetResult()
```

CLI/core 只负责命令解析、配置、文件上限、错误展示和生命周期。它不解析 SQL，也不包含
dialect 分支。一个进程单线程执行一个文件，没有后台 worker、queue、隐式重试或 driver
fallback。

内部 driver interface 为：

```c
typedef struct dbtool_schema_driver_ops {
  size_t struct_size;
  uint32_t abi_version;
  dbtool_status (*open)(void **out_context,
                        const dbtool_connection_config *config,
                        dbtool_error *error);
  dbtool_status (*apply)(void *context, const char *sql, size_t sql_size,
                         dbtool_apply_result *result,
                         dbtool_error *error);
  void (*close)(void *context);
} dbtool_schema_driver_ops;
```

该接口只存在于 source tree，不安装为通用数据库 ABI。SQLite 和 PostgreSQL 的 `main()`
分别直接注入静态 ops table；没有名称查找、动态注册或全局可变 driver state。

`apply()` 在调用期间借用只读 SQL bytes，driver 不保存该 view。成功和失败都必须结束或
drain native operation；`close()` 恰好释放一次 connection。

## CLI 契约

```text
turbodb-sqlite schema apply --database <path> --file <ddl.sql>
    [--max-script-bytes <n>] [--busy-timeout-ms <n>]

turbodb-postgresql schema apply --file <ddl.sql>
    [--conninfo-env <NAME>] [--max-script-bytes <n>]
```

`--file`、数据库配置和数值参数必须完整校验。缺失、重复或未知 option，零或溢出的
容量，不可读文件和超限文件都在连接或执行前失败。默认 script hard limit 为 16 MiB；
调用方可以显式调整。工具不读 stdin，避免无界输入和不可重放失败。

PostgreSQL 密码不作为普通 CLI 参数接收。连接信息来自 `--conninfo-env <NAME>`、libpq
标准环境或 `.pgpass`；错误输出不得回显连接串。

稳定退出码如下：

| 退出码 | 语义 |
|---:|---|
| 0 | 成功或 `--help` |
| 2 | 参数或调用契约错误 |
| 3 | 文件读取错误 |
| 4 | 连接或传输错误 |
| 5 | SQL 执行错误 |
| 6 | hard limit 超限 |
| 7 | 当前命令或结果类型不支持 |
| 8 | 内存不足 |
| 70 | 内部契约错误 |

## Driver 执行语义

SQLite 使用官方 [`sqlite3_exec`](https://sqlite.org/c3ref/exec.html) 直接执行完整文件。
工具不解析 transaction/savepoint control，也不补写或嵌套外层事务；`CREATE`、`DROP`
及其他 DDL 的提交与回滚边界由文件中的标准 `BEGIN; ... COMMIT;` 定义。

PostgreSQL 使用 libpq simple-query 发送完整文件，并按
[`PQsendQuery`](https://www.postgresql.org/docs/current/libpq-async.html) 契约通过
`PQgetResult()` drain 每个结果。工具与 SQLite driver 一样直接执行文件，不增加外层事务；
文件中的 `BEGIN; ... COMMIT;` 是唯一事务事实源。

## 数据、所有权与关闭

| 项目 | 契约 |
|---|---|
| 数据单元 | 一个有界 DDL SQL 文件 |
| 事实源 | 文件读取后由 invocation 独占的 buffer |
| 所有权 | CLI core 创建并释放；driver 只在 `apply()` 内借用 |
| 容量 | stat/read 前检查 `max_script_bytes` 和 `size + 1` overflow |
| 失败 | 立即返回 stage/native code；未提交的文件事务随连接关闭回滚，PostgreSQL drain results |
| 关闭 | apply 返回后释放 buffer 并关闭 connection；错误也走同一 cleanup |
| 线程模型 | 每个 invocation 单线程；context 不允许并发调用 |

## 错误语义与可观测性

`dbtool_error` 在 CLI 边界携带稳定 status、失败阶段、native code 和有界消息。内部层若
不能恢复，只向上传播错误；仅由 `main()` 输出一条 stderr 诊断。成功 stdout 只输出
driver、operation 和 statement count，不输出密码、连接串或 SQL 全文。

工具不重试、不切换 driver、不跳过失败 statement，也不把失败转换为成功。

## 构建与依赖

- `TURBODB_BUILD_DBTOOLS` 控制 standalone tools；cross-compiling 默认关闭。
- `TURBODB_DBTOOLS_WITH_SQLITE` 和 `TURBODB_DBTOOLS_WITH_PGSQL` 在 host build 默认开启，
  且独立于 ORM options；cross preset 显式关闭 dbtools 及其 PostgreSQL feature。
- 只有启用 PostgreSQL dbtool 时才加入 vcpkg PostgreSQL feature 并查找 libpq。
- targets 只链接 `Salts::Core` 与对应 native driver。
- executable 安装到 `CMAKE_INSTALL_BINDIR`，不导出为可链接 library target。
- `dbtools` install component 从最终 executable 扫描运行依赖；Windows 安装 DLL 到 `bin`，
  Unix 安装 shared library 到 `lib` 并使用 `$ORIGIN/../lib`。

## 风险与验证

- **HIGH / 事实**：复用 ORM raw query path 会使 SQLite 拒绝第二条 statement，并把
  PostgreSQL 的参数化单 statement 语义错误扩张为脚本语义。独立 native driver 是该问题的
  最小修复。
- **MED / 事实**：DBTools 不为缺少 `BEGIN; ... COMMIT;` 的文件伪造原子性。工具限定输入为
  trusted DDL，并逐个检查和 drain result；生成器负责输出标准文件级事务。
- **LOW / 事实**：新增 executable 和 CMake options 是加法行为；ORM public ABI、query、
  result 和 CFlow 行为不变。

验证范围：

1. CLI parser、文件上限、错误阶段和 cleanup 的 fake-driver TinyTest。
2. 真实 SQLite 临时数据库：标准事务内 `CREATE`/`DROP`、第二条失败回滚、重复 apply 和
   hard limit。
3. libpq test double：simple query、所有 results drain、intermediate error、connection
   error 和 secret redaction。
4. 显式 PostgreSQL live gate：执行 DDL 文件，再查询 catalog 和约束。
5. Windows Release configure/build/CTest/install/package smoke 与 EU Linux Docker 验证。
