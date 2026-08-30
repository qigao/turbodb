# Driver-specific database tools

## 背景与目标

TurboDB 当前的 SQLite 与 PostgreSQL 代码位于 ORM backend 内部。backend 直接消费
`orm_query_plan`，因此它们不是可独立使用的数据库驱动。SQLite query 路径还会拒绝
第二条 statement；PostgreSQL query 路径使用 `PQsendQueryParams()`，属于单 statement
的 extended query protocol。这些约束适合 ORM query Source，但不适合初始化脚本或
数据库数据搬运。

TBE 已能在构建期生成 SQLite/PostgreSQL bootstrap DDL。该输出只生成文件，不连接
数据库。新的工具层负责把这些构建产物应用到目标数据库，并为后续 schema-specific
数据导入/导出提供稳定边界；ORM、CFlow 和应用 query API 不承担部署职责。

目标如下：

- 生产实现保持 C11；C++ 不进入工具 target 的 production sources。
- 每个 executable 静态绑定一个 native driver，不使用运行时 registry 或 service
  locator。
- 第一批提供 `turbodb-sqlite` 和 `turbodb-postgresql` 的 `schema apply`。
- schema apply 直接链接 SQLite/libpq，不链接 `turbo_orm`、CFlow、CBind 或 CSerde。
- schema-specific 数据工具由 TBE 生成 model adapter，再与一个 driver 组合成独立
  executable；它们不通过 ORM 读写数据库。
- DataBind 只存在于逻辑记录格式边界。没有 data import/export 的普通 TurboDB 和
  schema apply 工具不查找、不链接 TurboParser。

非目标：migration diff、`ALTER TABLE` 规划、线上 migration history、通用 SQL shell、
数据库间在线复制、运行时加载任意 driver plugin。

## 已确认的边界

- **事实**：`orm/src/dbs/sqlite/backend.c` 使用 `sqlite3_prepare_v2()` 并检查 tail，
  非空 tail 返回 `SQLite query contains more than one statement`。
- **事实**：`orm/src/dbs/postgres/orm_postgres_libpq.c` 使用
  `PQsendQueryParams()`；该路径为参数化单 statement query 服务。
- **事实**：TBE database IR 当前只稳定暴露已转义 SQL table/column/type/constraint
  片段，适合 DDL 模板，不足以生成数据投影 adapter。
- **事实**：DataBind 能流式解析 schema-bound records 并提供只读 field getters；当前
  public API 没有从一组数据库 cells 构造 `DataBindRecord` 的 builder。
- **推论**：若导出端先手拼 JSON 再让 DataBind 重新解析，会复制数据、放大内存，并让
  JSON 成为其他格式的隐式事实源。该方案不作为正式边界。

SQLite 多 statement 执行采用官方
[`sqlite3_exec`](https://sqlite.org/c3ref/exec.html)；PostgreSQL 采用 libpq simple-query
发送与逐个 `PGresult` drain，遵循
[`PQsendQuery`](https://www.postgresql.org/docs/current/libpq-async.html) 契约。

## 分层架构

```text
TBE schema
  |-- tbe_compiler --lang <dialect> --------------------> bootstrap.sql
  `-- tbe_compiler --db-tool-source (第二阶段) ---------> model_adapter.c

bootstrap.sql -> turbodb-<driver> -> schema driver -> SQLite/libpq

model_adapter.c + data-tool core + one record driver
                                      |
                                      `-----------> generated executable
```

### CLI/core 层

CLI/core 只处理命令、配置优先级、文件边界、错误展示和生命周期。它不包含 SQL
dialect 分支。一个进程单线程执行一个命令，没有后台 worker、queue 或隐式重试。

配置优先级为命令行参数、显式命名的环境变量、native driver 默认值。PostgreSQL
密码不作为普通 CLI 参数接收；使用 `--conninfo-env <NAME>` 或 libpq 标准环境/.pgpass，
错误和日志不得回显连接串。

### schema driver 接口

第一阶段只定义最小的内部 interface：

```c
typedef struct dbtool_schema_driver_ops {
  size_t struct_size;
  uint32_t abi_version;
  int (*open)(void **out_context, const dbtool_connection_config *config,
              dbtool_error *error);
  int (*apply)(void *context, const char *sql, size_t sql_size,
               dbtool_apply_result *result, dbtool_error *error);
  void (*close)(void *context);
} dbtool_schema_driver_ops;
```

该 interface 是 source-tree internal contract，不安装为通用数据库 ABI。SQLite 和
PostgreSQL main 各自直接传入一个静态 ops table；没有名称查找、动态注册或全局可变
driver state。

`apply()` 输入是调用期间 borrowed 的只读 SQL bytes。driver 不保存该 view。成功和
失败都必须 drain/结束 native operation；`close()` 恰好释放一次 connection。

SQLite 使用 `sqlite3_exec()` 执行脚本。工具在外层 transaction 中执行，并通过 SQLite
authorizer 拒绝输入脚本自己的 transaction/savepoint control；任何 statement 失败即
rollback，因此不会留下已创建的前半部分 schema。

PostgreSQL 把整个 trusted bootstrap script 作为一个 simple-query request 发送，并用
`PQgetResult()` drain 每个 result。服务端对不含显式 transaction control 的多 statement
simple query 使用 implicit transaction；若输入自己包含 transaction control，则输入
脚本定义 transaction 语义。工具不声称能回滚脚本内已显式提交的副作用。

### record source/sink 接口

第二阶段再增加两个隔离接口，而不是扩张 schema driver：

- record sink：接收一个生成 adapter 已校验的 logical row，绑定并写入数据库；
- record source：推进 native cursor，并借出只在下一次 `next()`/`close()` 前有效的 cells。

每个 driver 明确声明 capability。SQLite/PostgreSQL 支持 schema、record source 和
record sink；Redis/Mongo/TidesDB 后续按各自数据模型增加工具，不伪装成 SQL table。
未实现的 capability 不出现在对应 executable 的 help 中，并返回明确的 unsupported
错误，不做 fallback。

### generated model adapter

DataBind 的 record API 是只读的，因此 model adapter 必须由 TBE 的数据库投影 IR 生成，
不能由 TurboDB 在运行时猜测。adapter 只包含 `db_table` message 的持久化字段，记录：

- TBE message/field 名与 native table/column 名；
- scalar kind、optional/default/generated 属性；
- dialect-specific value conversion（特别是 SQLite canonical decimal `uint64`、PostgreSQL
  `numeric(20,0)`、BLOB/bytea 和 UUID）；
- 导入 record -> bound cells 与 cursor cells -> 导出 record 的 generated callbacks。

生成代码依赖一个 versioned pure-C `dbtool_model_v1` contract，并链接
`TurboParser::DataBind`。它不包含数据库连接逻辑。一个 generated executable 只嵌入一份
schema/model adapter 和一个 driver，因此没有运行时 schema/driver registry。

## CLI 契约

第一阶段：

```text
turbodb-sqlite schema apply --database <path> --file <bootstrap.sql>
    [--max-script-bytes <n>] [--busy-timeout-ms <n>]

turbodb-postgresql schema apply --file <bootstrap.sql>
    [--conninfo-env <NAME>] [--max-script-bytes <n>]
```

`--file`、数据库配置和所有数值必须完整校验。缺失、重复、未知 option、零/溢出容量、
不可读文件或超限文件均在连接/执行前失败。默认 script hard limit 为 16 MiB；调用方可
显式调低或调高到实现定义的最大值。schema apply 不读 stdin，避免无上限输入和不可
重放失败。

第二阶段 generated executable：

```text
accounts-sqlite data import User --database accounts.db \
    --input users.json --format json
accounts-sqlite data export User --database accounts.db \
    --output users.json --format json
```

格式只在 adapter 完整支持时出现在 help。JSON 是 root array，CSV 是一份 header 加 rows，
YAML 是 root sequence，XML 是单一 records root，binary 是带长度 framing 的 TBE records；
各格式不互相 fallback。

## 数据、内存和关闭协议

### schema apply

| 项目 | 契约 |
|---|---|
| 数据单元 | 一个有界 SQL 文件 |
| 事实源 | `turbo_fs` 读取后由 invocation 独占的 buffer |
| 所有权 | CLI core 创建并释放；driver 只在 `apply()` 内借用 |
| 容量 | stat/read 前检查 `max_script_bytes` 和 `size + 1` overflow |
| 失败 | 立即停止，返回 stage/native code；SQLite rollback，PG drain 全部 result |
| 关闭 | apply 返回后释放 buffer，再关闭 connection；错误也走同一 cleanup |

### data import/export

数据面保持 single-threaded。输入按固定 chunk feed 给 DataBind stream；record callback 的
view 仅在 callback 内有效，generated adapter 必须在返回前完成 bind/copy。数据库 cursor
cell 只在下一次 cursor step 前有效，encoder 必须在 step 前消费。

所有增长点都有 hard limit：input bytes、chunk bytes、record bytes、row count、column
count、单 cell bytes 和 output bytes。默认 import 是单 transaction、全成功后 commit；
任一 parse/convert/bind/write 失败 rollback。若以后增加 batch commit，必须作为显式不同
模式并报告已提交 row count，不能静默把 all-or-nothing 改成 partial success。

导出到文件时先写同目录临时文件，成功 flush/close 后原子替换；失败删除精确临时文件，
既有目标保持不变。第二阶段不先提供 stdout，避免无法回滚的半个文档。

## 错误语义与可观测性

`dbtool_error` 在 CLI 边界携带稳定 tool status、阶段、native code 和有界消息。内部层若
不能恢复，只返回错误；只在 `main()` 消费一次并写 stderr。成功 stdout 只输出机器可读
摘要（driver、operation、statements/rows），不输出密码、SQL 全文或 record payload。

- invalid argument / limit / file / connection / SQL / constraint / conversion / I/O / internal
  为可区分状态；
- native SQLite/libpq detail 被复制到有界错误对象，不保存 driver-owned pointer；
- 不重试、不切换 driver、不跳过坏 record。

## 构建与依赖

- `TURBODB_BUILD_DBTOOLS` 控制 standalone tools；cross-compiling 默认关闭。
- `TURBODB_DBTOOLS_WITH_SQLITE` 和 `TURBODB_DBTOOLS_WITH_PGSQL` 独立于 `ORM_WITH_*`。
- 任一 PostgreSQL consumer 启用时才加入 vcpkg `postgresql` feature 并查找 libpq。
- 第一阶段 targets 只链接 `TurboUtils::Core` 与对应 native driver。
- 第二阶段 generated target 才精确从 `TURBOPARSER_ROOT` 查找
  `TurboParser::DataBind` 和 installed `tbe_compiler`；普通构建不要求 `TURBOPARSER_ROOT`。
- executables 安装到 `CMAKE_INSTALL_BINDIR`，不作为可链接 imported library targets 导出。

## 风险、兼容性与回滚

- **HIGH / 事实**：若把 ORM raw query path直接用于脚本，SQLite 会拒绝第二条 statement，
  PostgreSQL extended query 也不是脚本 contract。最小修复是独立 native schema driver。
- **HIGH / 推论**：没有 generated projection adapter 时手写导出 JSON 会产生双事实源和类型
  漂移。data export 在 adapter 合入前不暴露。
- **MED / 事实**：PostgreSQL 输入若包含显式 `COMMIT`，脚本可改变 implicit transaction
  语义。CLI 文档限定 trusted bootstrap input，并逐个检查/drain result；不承诺撤销脚本
  自己已经提交的副作用。
- **MED / 推论**：新增 TurboParser runtime 为所有 TurboDB 用户的强制依赖会破坏现有 package
  边界。DataBind 依赖因此只落在 generated data tool target。
- **LOW / 事实**：新增安装 executable 和 CMake options 是加法行为；ORM public headers、ABI、
  query/result/CFlow 行为不变。

第一阶段回滚只需移除 `dbtools/` subdirectory/options/install entries，不影响 ORM。第二阶段
回滚可停止生成 model adapter 并删除 generated targets；bootstrap DDL 和 schema apply 仍可
独立使用，数据库数据格式不迁移。

## 验证范围

1. CLI parser、文件上限、错误阶段和 cleanup 的 fake-driver TinyTest。
2. 真实 SQLite 临时数据库：多 statement 成功、第二条失败全回滚、transaction control 拒绝、
   重复 apply 错误和 hard limit。
3. libpq test double：simple query 发送、所有 results drain、intermediate error、connection
   error 和 secret redaction。
4. 显式 PostgreSQL live gate：TBE 生成 DDL 应用到一次性 container，再查 catalog/约束。
5. Windows Release configure/build/CTest/install/package smoke；EU Linux Docker 同等验证。
6. generated data tools 后续覆盖每种 scalar/NULL/最大值、format round trip、输入中途失败
   rollback、输出临时文件原子替换和所有容量边界。
