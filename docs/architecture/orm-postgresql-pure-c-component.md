# Pure-C PostgreSQL Component and Composite-Key Contract

## 背景

当前 ORM 核心已经迁移为 C11，并通过 CFlow Source 暴露有界 reactive row stream。历史分支 `feat/orm-pg-control` 仍然修改 `orm_c.cpp`、`backend.cpp` 和已经删除的 C++ schema generator；将它整体合入当前主线会重新引入生产 C++ 实现，并产生多处 modify/delete 冲突。

仍需保留的需求有三项：

1. PostgreSQL/libpq 依赖必须位于可选组件边界，普通 `Orm::C` 消费者不应发现 PostgreSQL。
2. PostgreSQL 必须有显式启用、真实服务器支持的 live integration gate。
3. 复合主键必须能通过当前纯 C query API 原子地形成多列等值谓词，供手写 facade、CMeta/CBind 描述符适配器或 build-time generated C code 使用。

## 候选方案

### 方案 A：rebase 历史分支

放弃。历史分支以 C++ core、全局 driver registration 和仓库内 C++ schema generator 为基础，与当前 C11 core、header-only C++ wrapper 和 CFlow Source 所有权契约冲突。解决冲突后得到的代码也不是当前目标架构。

### 方案 B：进程级 PostgreSQL 注册表

放弃。`orm_postgresql_register()` 会引入进程级可变状态、初始化顺序和并发注册语义。它隐藏依赖，测试必须清理全局状态，也使同一进程难以承载不同 driver 实例。

### 方案 C：显式 PostgreSQL connector + 原子 key parts

采用。PostgreSQL 组件暴露 `orm_postgresql_connect()`，调用方显式选择组件；核心不通过全局注册表寻找它。复合主键使用有界 `orm_key_part_t[]`，一次调用把所有 key parts 复制进 query plan，失败则回滚到调用前状态。

## 公开接口

核心增加以下 value type 和操作：

```c
typedef struct orm_key_part {
  orm_string_view_t column;
  orm_value_t value;
} orm_key_part_t;

ORM_C_API orm_status_t ORM_C_CALL orm_query_where_key(
    orm_query_t *query, const orm_key_part_t *parts, uint32_t part_count,
    orm_error_t *error);
```

`part_count` 必须大于零；每个 column 必须是合法结构化标识符；列名不得重复。所有列都使用 `ORM_COMPARE_EQUAL`。函数复制 column 及 text/blob payload，调用返回后输入数组和借用值即可失效。若参数、容量或分配失败，query 的 predicate count 和 parameter-byte accounting 保持调用前状态。

PostgreSQL component 增加：

```c
ORM_POSTGRESQL_API orm_status_t ORM_C_CALL orm_postgresql_connect(
    const orm_config_t *config, orm_connection_t **out_connection,
    orm_error_t *error);
```

该函数只接受 `postgres` 或 `postgresql` driver token。成功后 connection 拥有 `PGconn`；`orm_disconnect()` 仍是唯一释放入口。PostgreSQL query Source 的现有契约不变：成功 open 后 Source 拥有 cursor，query、connection、row descriptor 和 scheduler 相关对象必须存活到 Source close。

C++ 只提供 header wrapper。`orm::connection` 增加接收 C connector function pointer 的构造函数，`orm_postgresql.hpp` 以内联函数调用 `orm_postgresql_connect()`；没有 `.cpp` 生产源。

## 内部组件边界

`Orm::C` 继续拥有 connection、query plan、materialized result、transaction façade 和通用 CFlow Source 协议。`Orm::PostgreSQL` 拥有 libpq connection、parameter encoding、SQLSTATE mapping 和 PostgreSQL cursor adapter。

两者由同版本 SDK 一起构建。core 向 component 暴露一个只在私有头声明的 versioned connector hook，以及 PostgreSQL backend 当前需要的通用错误/view/SQL-render services。这些符号不安装为 driver-author API，不承诺跨 TurboDB 版本兼容；公开 ABI 仍只有 `orm.h` 和 `orm_postgresql.h`。

## 构建与依赖

- `ORM_WITH_PGSQL=OFF`：不调用 `find_package(PostgreSQL)`，不构建 `Orm::PostgreSQL`。
- `ORM_WITH_PGSQL=ON`：构建纯 C `orm_postgresql` target 和 `Orm::PostgreSQL` alias；`Orm::C` 本身不链接 libpq。
- shared core 的生产 target 和文件名统一为 `turbo_orm`。安装消费者始终使用稳定 target `Orm::C`，不依赖仓库内部 target 名或物理文件名。
- `Orm` 是唯一的消费端 CMake package；启用 PostgreSQL 时，同一个
  `OrmTargets.cmake` 额外导出 `Orm::PostgreSQL`，不生成独立 driver package。
- core 和 component 只提供 shared SDK。component 自己封闭 libpq 的 runtime
  dependencies，消费端不执行 `find_dependency(PostgreSQL)`。
- CBind/CSerde/CFlow 继续来自 TurboUtils。ORM 不新增 TurboParser runtime 依赖，也不在运行时调用 `tbe_compiler`。

## DataBind/CBind 关系

复合主键属于 query construction，不属于 wire-format serialization。ORM 因此不解析 schema 文件，也不复制 DataBind 的 descriptor/runtime。

需要 typed facade 时只允许两条外部路线：

1. build/CI 使用 `tbe_compiler --source-output` 生成 `.h/.c`，生成代码把 key members 转成 `orm_key_part_t[]`；
2. 现有 C struct 使用 `TBE_TYPED_DEFINE_STRUCT` 描述，应用层 adapter 读取明确的 key metadata 并构造 `orm_key_part_t[]`。

两条路线都在调用边界完成值转换；ORM 立即复制 key parts，不保存 DataBind object、descriptor child 或 callback-borrowed view。

## Live test gate

新增 `ORM_POSTGRES_LIVE_TESTS`，默认 `OFF`。启用时必须同时满足：

- `ORM_BUILD_TESTS=ON`；
- `ORM_WITH_PGSQL=ON`；
- configure 进程存在非空 `TURBODB_ORM_PGSQL_TEST_CONNINFO`。

任一条件缺失都在 configure 阶段失败，不注册一个运行时 skip 的假测试。CTest 继承同名环境变量；测试不得打印 conninfo 或密码。

live test 使用独立 connection 和 PostgreSQL temporary table，覆盖 text/bytea、复合键查询、transaction commit/rollback、SQLSTATE 与 result limits。所有资源沿单一 cleanup 路径释放；测试失败也不得修改已有 schema 或表。

## 状态、失败和关闭

- Query plan 是 predicate 的唯一事实源；`orm_query_where_key()` 不建立镜像 key state。
- Batch append 以原 predicate size 和 parameter bytes 为 checkpoint；每个成功 push 最终保留，或在失败时 pop/destroy 并恢复 accounting。
- Component connection 创建失败时释放 `PGconn`、清空 output，并保留第一条有用错误。
- Live test 的 temporary table 随 connection 关闭自动消失；transaction destroy 对 active transaction 执行 rollback。
- 不引入 backend fallback。PostgreSQL component 不可用或 driver token 不匹配时返回明确错误。

## 兼容性和迁移

`orm_query_where_key()` 是加法 API。PostgreSQL 构建从嵌入 `Orm::C` 改为 `Orm::PostgreSQL` 是依赖边界变化：启用 PostgreSQL 的 C 调用方需要包含 `orm_postgresql.h` 并调用 `orm_postgresql_connect()`；CMake 调用方仍只 `find_package(Orm)`，并链接 `Orm::PostgreSQL`。普通 SQLite/Redis/Mongo/TidesDB 调用不变。

回滚时可删除 component target/header/hook，并恢复 PostgreSQL sources 到 `ORM_C_SOURCES`；composite-key API 与 live test 没有必要随 component 回滚。

## 验证范围

1. Windows Release core 全量测试。
2. Windows Release PostgreSQL component configure/build、unit test、C/C++ header consumer 和 install package consumer。
3. composite-key 原子成功、重复列、非法列、容量失败与状态回滚。
4. EU Ubuntu Docker 中构建 core/component，并连接一次性 PostgreSQL container 跑 live gate。
5. `git diff --check`、安装 header/targets/config 检查，以及生产 target source extension 检查。
