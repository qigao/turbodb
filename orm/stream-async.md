# ORM Stream / Reactive 与异步查询

## 使用入口

安装包提供 `<orm.hpp>` 与 `Orm::Cpp`。C 调用方使用 `<orm.h>`、
`orm_query_open_async_flow()` 和 Salts CFlow Publisher/Subscription。

C++ `query.open<Row>(shape)` 返回拥有查询的 Publisher；对右值 Publisher
调用 `.pipe()`，可组合 `.filter(predicate).map(transform).take(count)`，
最后 `.subscribe(scheduler, subscriber)` 获得 `orm::subscription`。
`request(n)` 增加下游需求；`cancel()` 停止交付；`close()` 释放执行资源。
操作符在客户端执行，筛选不会改写 SQL。数据库端筛选仍通过查询构造接口表达。

`filter` / `map` 接受 CFlow callable；自定义 Row 使用 Salts
`cflow_function_typed_filter_projection_admit` / `cflow_function_typed_adapter_projection_admit`
生成的 typed projection。输入、输出描述符与实际 Row 布局必须一致。
失败的投影或订阅 admission 抛出 `orm::flow_error`；ORM admission 错误抛出
`orm::status_error`。执行阶段错误通过 Subscriber 的 `on_error` 交付，并可从
Subscription 的 `status()` / `error()` 读取。

完整可编译示例见 [C++ 管道契约测试](tests/flow/sqlite/orm_cpp_flow_test.cpp)：
包含 Row 描述符、typed filter/map、Subscriber、scheduler、需求和释放顺序。
该文件由 `orm_cpp_flow_test` 与 `orm_owner_cpp_flow_test` 两个目标构建运行。

## 异步行查询入口

`orm_query_open_async_flow(query, flow_config, async_config, out, error)`：

- `query`：非事务行查询；MySQL 创建独立原生会话，PostgreSQL/SQLite 使用当前连接。
- `flow_config`：与同步 Flow 相同的 Row 描述符及解码资源上限。
- `async_config.struct_size`：设置为 `sizeof(orm_async_config_t)`。
- `scheduler`：借用的单 owner、支持延迟任务的 scheduler，必须与 Subscription
  使用同一实例。并发 worker scheduler 和不支持延迟的 scheduler 会被拒绝。
- `poll_interval_ticks` / `timeout_ticks`：非零；单位由 scheduler 定义。
  deadline 从打开查询时开始，包括无需求期间；到下一次推进时检查。
- `out`：必须为空；成功后拥有 Publisher，失败时不转移查询所有权。
- `error`：通过 `orm_error_init()` 初始化，保留 admission 失败原因。

C++ 对应 `.open_async<Row>(shape, async_config)`，之后使用相同的 `.pipe()`。
建连和事务控制保留同步入口；当前异步入口不处理事务、写命令或自动重试。
Redis、TidesDB 等尚未实现异步查询的驱动返回 `ORM_STATUS_UNSUPPORTED`。

### MySQL / PostgreSQL 原生非阻塞 I/O

MySQL 使用 CNet 的零等待 `poll(0)` 推进 TLS、认证、预处理和逐行读取。
独立驱动 `<turbodb_mysql.h>` 同时提供 `mysql_session_start_async_source`、
`mysql_session_async_next`、`mysql_session_async_cancel`、`mysql_session_async_close`。
`next` 每次最多推进一次 poll，返回 WAIT / METADATA / ROW / DONE / ERROR。
SQL、参数和 config 借用到 METADATA 或关闭，metadata 借用到关闭，row 借用到下一次推进。

PostgreSQL 使用 `PQsetnonblocking(1)`、`PQsendQueryParams`、`PQflush`、
`PQconsumeInput`、`PQisBusy` 与 single-row 模式，仅在结果就绪时调用 `PQgetResult`。
参见 [libpq 异步命令契约](https://www.postgresql.org/docs/current/libpq-async.html)。
游标存活期间连接被独占，另一个查询或事务控制返回 `BUSY`；完整消费并销毁后恢复阻塞模式。

网络暂未就绪时返回携带 timer waitable 的 WAIT，CFlow scheduler 延迟唤醒后再检查。
这是定时驱动的原生非阻塞 I/O，不是 socket readiness 注册；轮询间隔决定额外延迟与检查频率。
Salts 的 test scheduler 使用虚拟时间。生产应用应提供与自身事件循环衔接、满足上述能力的
单 owner scheduler，不应把虚拟时间当作墙上时钟。

### SQLite 后台执行

SQLite 通过相同的 C/C++ 异步入口使用 Salts CFlow 串行 Executor。
每个异步游标拥有一个工作线程、容量为 1 的任务队列和一个结果槽位；
有下游需求时才提交任务。第一次任务准备并绑定 SQL，之后每次任务最多
推进一行；没有需求时不预读下一行。后台仍调用同步 SQLite API，调用方
通过 timer WAIT 等待结果。驱动声明 `OWNER_EXECUTOR`，不声明 `NATIVE_WAIT`。

连接、查询计划和参数由已有 ORM/插件游标所有权保留；后台借用计划、复制资源限制。
工作线程完成后用 release/acquire 发布行 reader；调用方解码期间不提交下一任务。
当前 statement 是行数据的事实源，列名和文本/blob 视图只借用到本次同步解码结束，
不另建结果队列。每行成本与列数及当前行字节数相关，累计结果行数/字节数仍受连接配额限制。
SQL 准备、绑定和 step 的错误在异步推进时交付；任务入队失败返回明确错误，不退回同步执行。

从异步打开成功到游标销毁，连接保持独占；其他查询、异步游标和事务开始返回 `BUSY`。
已有同步游标或事务也会阻止异步打开。SQLite 连接采用 `SQLITE_OPEN_FULLMUTEX`；
异步入口拒绝缺少连接 mutex 的单线程 SQLite 环境，原有同步入口仍可使用。
应用不得通过全局配置禁用 SQLite 线程支持。
参见 [SQLite 线程模式](https://www.sqlite.org/threadsafe.html)。

取消设置原子停止标记并请求 `sqlite3_interrupt()`，progress handler 同时处理
中断发生在 prepare/step 之前的竞态。超时交付 `ORM_STATUS_SQL_ERROR` 和明确的 deadline
错误消息；清理完成后连接可以复用。取消不保证任意 SQL 副作用回滚。
销毁会等待 Executor 退出，再清理 statement、progress handler 和连接独占状态，
最后释放借用的计划和连接。自定义 SQL 函数、VFS I/O 或 busy handler 未返回时，
销毁可能等待；异步不承诺控制面关闭无阻塞，也不会提前释放工作线程仍引用的资源。
参见 [SQLite progress handler](https://www.sqlite.org/c3ref/progress_handler.html)。

## 所有权、背压与关闭

状态事实源是驱动游标：协议阶段、当前行、累计配额只在该游标推进。
Publisher 负责解码，Subscription 负责需求、WAIT 注册和终态交付。
Pipeline/Subscription 可移动但不可复制，堆上的 Graph 地址保持稳定。
描述符、callable 捕获、Subscriber 上下文和 scheduler 必须存活到 `close()`。
`close()`、销毁与移动赋值应在回调和 scheduler pump 之外进行。

没有下游需求时不拉取行；MySQL/PostgreSQL 在异步打开时已开始连接/发送查询，
SQLite 则将 SQL 准备和执行推迟到首次需求。
每个活跃查询持有一个 timer 和有界驱动缓冲区；行数、累计结果字节、列数、
metadata 和命令长度沿用连接限制，满额返回明确错误，不自动扩容绕过配额。
每次 resume 的网络/工作完成检查为常数次；行解析和映射成本与当前行大小相关。
跨线程并发操作同一查询、游标或 Subscription 不受支持。

PostgreSQL 取消未完成查询会关闭原生连接，避免阻塞等待服务器排空结果；
调用方须新建连接。超时/网络错误同样结束该查询，不代表 SQL 副作用已回滚。
MySQL 取消关闭该查询的独立会话。关闭属于控制面，可等待 CNet drain，
不承诺整个生命周期都不阻塞。

MySQL `async_close` 使用连接配置的 `timeout_ms` 作为 drain 上限；失败返回
TIMEOUT/IO，保留 handle 和输入所有权，可再次调用 close。关闭开始后不可继续 next。
void `async_destroy` 和 ORM 析构无法返回仍活跃的 callback owner，因此 drain 失败且
CNet 仍持有资源时 fail fast 终止进程，绝不释放仍被回调引用的内存。
这也是同步会话构造失败清理必须遵守的约束；不要在 CNet 回调中销毁会话。

## 设计取舍与兼容

采用 ORM 核心 + 可选驱动 ABI 扩展 + CFlow Reactive 的分层方案。
相较把同步调用丢给线程池，原生推进可在等待期间归还执行权；代价是显式的
调度、超时、取消和驱动状态机。相较另建事件循环，复用 Salts timer/waker
避免再维护一套并发队列，但当前需要定期检查网络。

SQLite 标准接口没有相同的非阻塞查询能力，因此选择隔离的 Salts 串行 Executor。
相较在订阅线程执行同步 step，它可隔离查询等待；相较全局共享线程池，它避免连接间
互相阻塞和新增全局生命周期。代价是每个活跃异步游标占用一个工作线程，打开/销毁
可能等待线程启动/退出。没有新增公共 ABI 字段或第三方依赖，仍使用现有资源限制。
回滚 SQLite 功能可在调用处恢复同步 `open()`；存在活跃后台任务时必须先销毁对应订阅。

宿主只接受 Driver ABI 2 与 `TurboDb.Driver` 契约版本 4，旧插件加载失败，
不提供双 ABI 适配。连接操作表使用 `orm_driver_connection_ops_v2`，必须包含完整的
`open_async_cursor` 字段；不支持异步的驱动可将回调设为 NULL。
其他未改变布局的 DTO 保留 `_v1` 类型名，其 header 同样必须声明 Driver ABI 2。
类型名后缀表示布局版本，不代表宿主仍接受旧 Driver ABI。
ORM 核心仍不链接数据库库，C++ 层不复制查询执行逻辑，也不新增外部依赖。

迁移时统一重编 ORM、所有驱动与使用 Driver SDK 的客户端，并成套替换安装包。
查询可逐个选择 async，原有同步查询入口不变。回滚必须成套恢复旧核心和旧驱动，
不能混用两个 ABI 的模块，也不能在已发送查询中途切换执行模型。
ABI、错误状态和资源预算保持可测试；PostgreSQL 取消关闭连接、MySQL drain
失败的 fail-fast，以及额外轮询延迟是调用方必须接受的公开行为边界。

## 验证

`cmake --build --preset ci-windows-release` 和 `ctest --preset ci-windows-release`
覆盖普通单元测试；TinyMock 用例验证 zero-time poll、WAIT、deadline、取消、
关闭重试、旧 ABI/契约及截断连接操作表的拒绝；C++ 测试覆盖需求、移动、typed filter/map 和 admission 失败。

`orm_sqlite_async` 验证真实后台线程、WAIT、取消/超时、连接独占、结果配额和
TinyMock 入队拒绝；`orm_sqlite_plugin` 验证真实插件的异步需求和资源保留，
`orm_cpp_flow` / `orm_owner_cpp_flow` 验证 C++ 异步管道和连接复用。

开启 `BUILD_E2E_TESTS` 后，MySQL/PostgreSQL 既有 E2E 目标都会执行
[异步行查询与背压测试](tests/support/async_query.h)。它经过公开 ORM API、
插件边界、真实驱动和 CFlow Subscription，复用各驱动 E2E 的连接环境变量。
E2E 需要可连接的测试数据库；仅编译成功不能替代真实服务验证。
