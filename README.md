# TurboDB

TurboDB 提供纯 C 数据库组件、C++ header-only 包装和按 native driver 构建的数据库工具。
ORM、Redis、TidesDB 与 standalone tools 分别由 CMake 选项控制。

## Standalone DDL SQL tools

`turbodb-sqlite` 与 `turbodb-postgresql` 可批量执行数据库初始化 SQL，且不经过
ORM。二者只通过 driver 执行 DDL SQL 文件，命令为 `schema apply`；它们不是 migration
diff/history 管理器，也不会
加载运行时 driver plugin。

Windows 默认 Release 配置构建 SQLite 与 PostgreSQL 工具：

```powershell
cmake --fresh --preset win-release-user
cmake --build --preset win-release-user --target turbodb-sqlite turbodb-postgresql
cmake --build --preset install-win-release-user
```

执行 SQLite bootstrap：

```powershell
turbodb-sqlite schema apply `
  --database .\app.db `
  --file .\bootstrap.sqlite.sql
```

PostgreSQL 连接串只通过调用方命名的环境变量传递，避免出现在普通命令行参数和错误
输出中；不指定 `--conninfo-env` 时使用 libpq 的标准环境、service 与 `.pgpass`：

```powershell
$env:APP_PG_CONNINFO = 'host=127.0.0.1 dbname=app user=app'
turbodb-postgresql schema apply `
  --file .\bootstrap.postgresql.sql `
  --conninfo-env APP_PG_CONNINFO
```

默认脚本上限为 16 MiB，可用 `--max-script-bytes` 显式调整。SQLite 还支持
`--busy-timeout-ms`，默认 5000 ms。输入文件必须非空、可完整读取且不含嵌入 NUL；工具
不读 stdin、不重试、不切换 driver。

DDL 文件负责定义标准事务边界，例如 `BEGIN; ... COMMIT;`。SQLite 与 PostgreSQL 工具都
直接执行文件，不解析、补写或嵌套外层事务。SQLite 使用一次 `sqlite3_exec()`；PostgreSQL
使用一次 libpq simple query 并释放所有 `PGresult`。`CREATE` 与 `DROP` 的原子性完全由
文件中的事务定义。

退出码：`0` 成功/帮助，`2` 参数错误，`3` 文件错误，`4` 连接错误，`5` SQL 错误，
`6` 超限，`7` 不支持，`8` 内存不足，`70` 内部错误。

PostgreSQL ORM backend 与 PostgreSQL dbtool 默认构建；libpq 只作为对应实现 target 的
private/runtime 依赖，不进入使用方的编译或链接接口。安装只暴露 dbtools executable，
不要求使用方 `find_package(TurboDB)`。`dbtools` install component
会携带运行所需的动态库闭包；DDL SQL tools 不链接 `turbo_orm`、CFlow、CBind、CSerde 或
Salts parser targets。详细设计与验证边界见
[driver-data-tools.md](docs/architecture/driver-data-tools.md)，EU Docker 验证见
[TURBODB_LINUX_REMOTE_TEST_RUNBOOK.md](docs/TURBODB_LINUX_REMOTE_TEST_RUNBOOK.md)。
