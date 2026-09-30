# dbtools 与数据库驱动插件

`turbodb-sqlite`、`turbodb-postgresql`、`turbodb-mysql` 通过已安装 SDK 的 `Salts::Plugin`
加载数据库驱动。工具程序不直接链接 SQLite、libpq 或 ORM；数据库连接和脚本
执行由 `turbodb_driver_sqlite`、`turbodb_driver_postgresql`、`turbodb_driver_mysql` 模块负责。
同一个模块同时导出 `TurboDb.Driver` 和 `TurboDb.SchemaApply`，SQLite 还保留
原有维护接口。

## 使用

插件路径必须是绝对路径。优先使用 `--plugin`，否则读取
`TURBODB_SQLITE_PLUGIN`、`TURBODB_POSTGRESQL_PLUGIN` 或 `TURBODB_MYSQL_PLUGIN`。不搜索目录，不自动选用
其他 ABI 或驱动。`--help` 不加载插件。

参数绑定使用已安装 SaltsUtils 的 `Salts::CmdParser`。选项描述同时用于前置校验，
使重复、未知、缺值参数继续返回 dbtools 错误，而不会触发解析库的进程退出。
数值仍按正十进制及原有范围校验，帮助退出码为 0、参数错误为 2。
`@` 开头的选项值保留为普通字符串，不展开 response file。
CmdParser 会加载工作目录的 `.env`，已有进程环境变量优先；插件路径仍优先使用 `--plugin`。

在 VS 开发环境中使用仓库 preset 构建：

```powershell
cmake --preset win-release-user
cmake --build --preset win-release-user --target turbodb-sqlite turbodb-postgresql
$plugin = (Resolve-Path build/Msvc-Release/bin/turbodb_driver_sqlite.dll).Path
build/Msvc-Release/bin/turbodb-sqlite.exe schema apply `
  --plugin $plugin --database example.db `
  --file dbtools/tests/sqlite/fixtures/sqlite_schema.sql
```

运行环境需要能找到已安装 Salts、SaltsUtils 及驱动所需的本机数据库动态库。
CTest preset 已配置相应运行路径；直接运行工具时也要设置相同的 `PATH`
（Linux 使用 `LD_LIBRARY_PATH`）。连接密钥继续通过 PostgreSQL 的
`--conninfo-env <变量名>` 传入，工具错误不输出连接串。

既有 `schema apply` 参数、退出码、错误阶段和语句计数保持不变；运行时新增
插件路径配置。SQL 仍整段提交给数据库，不按分号拆分，不隐式补事务，也不重试。
原生数据库负责事务语义；脚本包含事务时，失败后的连接关闭会结束未提交事务。
没有事务的脚本可能已经提交前面的语句，工具不承诺撤销这些结果。

## MySQL

```powershell
cmake --build --preset win-release-user --target turbodb-mysql
$env:MYSQL_SCHEMA_CONNECTION = @'
{"host":"127.0.0.1","port":3306,"username":"schema_user","password":"replace-me","database":"app","ca_file":"C:/certs/ca.pem","server_name":"db.example","timeout_ms":10000}
'@
$plugin = (Resolve-Path build/Msvc-Release/bin/turbodb_driver_mysql.dll).Path
build/Msvc-Release/bin/turbodb-mysql.exe schema apply `
  --plugin $plugin --conninfo-env MYSQL_SCHEMA_CONNECTION --file schema.sql
```

`--conninfo-env` 对 MySQL 必填。配置是 JSON 对象：`host`、`username`、`password`、
`database`、`ca_file`、`server_name` 是必填字符串，只有 password 允许空串。
`port` 默认 3306，范围 1–65535；`timeout_ms` 默认 5000，范围 1–4294967295，
两者必须是十进制正整数。未知字段、类型错误、内嵌 NUL 都会被拒绝。
JSON 重复键遵循 Salts JSON parser 的最后一个值生效规则，应避免重复键。
连接文档最多 32768 字节，每个字符串最多 4096 字节；错误不输出连接文档内容。

MySQL 的 `open` 校验并持有配置；每次 `apply` 创建一个独立的、校验 CA 和服务器名称的
TLS 会话，返回前关闭连接。整个脚本在同一会话中执行；不同 apply 不共享临时表或事务。
复用的是根目录 `mysql/` 通用驱动，没有引入 libmysqlclient。

SQL 文件整段作为 COM_QUERY 发送，并协商 `CLIENT_MULTI_STATEMENTS` 和
`CLIENT_MULTI_RESULTS`。按服务器 OK 响应计数，直到最后一个结果；遇到 SQL 错误、
返回行或 LOCAL INFILE 请求立即失败并关闭连接，不发送本地文件。失败时计数为 0，
但服务器可能已执行或提交部分语句，不能据此重试。协议依据见
[MySQL 多语句执行](https://dev.mysql.com/doc/c-api/8.0/en/c-api-multiple-queries.html)。

脚本同时受 `--max-script-bytes` 和原生单命令上限 **16777213 字节**约束。
不解释 mysql 客户端的 `DELIMITER` 指令；存储程序定义应直接使用服务器 SQL 语法。
MySQL DDL 有隐式提交行为，脚本中的 BEGIN 不能保证 DDL 整体回滚，参见
[MySQL 隐式提交](https://dev.mysql.com/doc/refman/8.0/en/implicit-commit.html)。

MySQL 单元测试在 `dbtools/tests/mysql`，协议测试在 `mysql/tests/session`。
E2E 单独放在 `dbtools/tests/mysql/e2e`，由 `BUILD_E2E_TESTS` 控制，使用
`TURBODB_DBTOOLS_MYSQL_TEST_CONNINFO`（同样的 JSON 配置）；仅工具构建还需
`TURBODB_MYSQL_PLUGIN`。测试只使用临时表，但仍须连接专用测试实例。

## 接口与生命周期

公共契约位于 [drivers/schema](../drivers/schema/dbtool_schema_driver.h)，独立于
ORM 和 CLI 实现。`TurboDB::SchemaABI` 只发布头文件与 Salts Plugin ABI 依赖。
`TurboDb.SchemaApply` 的 `operations()` 返回版本化的 `open/apply/close` 表，
表的布局、输入输出类型和错误值由 schema contract version 1 固定。

- `dbtool_plugin_load` 接受零初始化句柄、绝对模块路径和预期驱动 ID，验证
  Plugin ABI、接口 ID、契约版本、CMeta 接口形状及必需操作；失败仍须调用 close。
- 加载器持有一个模块 lease，覆盖操作表、连接和调用期间所有插件内存。
  配置和 SQL 只借用到对应调用返回；结果和错误缓冲区由调用方持有。
- 插件创建并释放自己的连接，成功 open 必须匹配一次 close。单个连接上的
  操作串行执行；不得在连接或其他借用仍存活时关闭宿主句柄。
- 先关闭连接，再释放 lease、请求停止、确认静默、卸载模块，最后销毁 registry。
  关闭失败保留未完成状态并返回错误，调用方可以重试，不能继续使用失效操作表。
- 加载器 registry 容量固定为 1，不加载其他插件，也不改变 ORM runtime 的状态。

## 构建、安装与验证

`TURBODB_BUILD_DBTOOLS` 和 `TURBODB_BUILD_ORM` 保持独立。
同时开启时，构建工具目标也会构建对应驱动；安装 `dbtools` component 会包含
工具、SQLite/PostgreSQL/MySQL 驱动及它们依赖的 ORM 核心。
仅构建 dbtools 时不构建 ORM，运行时通过上述路径配置使用已有驱动及其依赖。
纯工具构建关闭测试后不需要 SQLite/libpq 开发包；原生后端测试仍需要这些包。

驱动作为 CMake MODULE 安装在 `${CMAKE_INSTALL_LIBDIR}/turbodb/drivers`
（Windows 默认也是 `lib/turbodb/drivers`）。依赖库仍由部署环境提供。
schema ABI 头文件安装在 `include/turbodb/schema`，归属 `SchemaABI` component。

```powershell
cmake --build --preset win-release-user
ctest --preset win-release-user -R "^dbtool_"
cmake --preset win-release-dbtools-user
cmake --build --preset win-release-dbtools-user
ctest --preset win-release-dbtools-user
```

测试覆盖原生 SQL 行为、SQLite 事务回滚、PostgreSQL 多结果排空、插件 ABI 和
契约拒绝、模块身份、lease 与可重试关闭；完整配置还直接消费真实 ORM 驱动。
PostgreSQL E2E 使用 `TURBODB_DBTOOLS_PG_TEST_CONNINFO`；仅工具配置运行 E2E 时还
须提供 `TURBODB_POSTGRESQL_PLUGIN`。E2E 会修改测试数据库，只应指向专用测试实例。

## 设计决策与迁移

原先 dbtools 拥有第二套静态数据库后端，工具和 ORM 驱动分别链接数据库 SDK。
直接改用 ORM 查询不能保留整段 DDL 脚本语义；单独创建一套工具插件又会复制
数据库模块的构建和部署边界。因此选择在同一驱动模块增加可选 schema 导出。

现有 schema 实现移动到 `drivers/{sqlite,postgresql}/schema.c`，没有改写执行
算法。加载、身份和 ABI 校验交给 Salts；宿主只管理一个有界 registry 和 lease。
MySQL 新增 `mysql_session_execute_script`，独立的脚本 action 复用原有 TLS/认证、
收发和关闭状态机，普通 ORM 预处理语句不启用多语句执行。相比另引入 libmysqlclient，
这保留了通用驱动的依赖和安全边界；代价是单命令大小受当前包编码边界限制。
新增 MySQL 类型不改变 schema v1 数据布局或既有枚举值。

代价是增加模块部署和一次加载/校验开销，工具需要配置插件路径。ORM 原有接口
和调用语义不变，缺少 schema 导出的旧驱动会在连接前被明确拒绝。

迁移时先部署带 schema 导出的驱动，再更新工具并配置路径，验证后替换旧工具。
回滚需恢复旧工具与其静态后端构建；数据库格式没有变化，无需数据迁移。
