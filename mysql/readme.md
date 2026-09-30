# MySQL 通用客户端

`TurboDB::MySQL` 提供独立于 ORM 的静态客户端库。`mysql/` 拥有协议编解码、
认证、CNet 会话、参数绑定、事务、逐行结果和 CSerde 行读取；`drivers/mysql/`
负责 ORM 查询计划、错误转换、游标适配、schema 工具适配及 Salts 插件入口。

客户端包含 `turbodb_mysql.h` 并链接 `TurboDB::MySQL`，不需要 ORM 头文件、
`Orm::C` 或插件加载器。ORM 插件复用同一客户端库，不再编译自己的协议副本。
本库使用已安装的 Salts（CNet、CSerde）及已有 OpenSSL 依赖。

可编译的 C/C++ 消费用法见 [C 测试](tests/session/mysql_client_test.c) 和
[C++ 测试](tests/session/mysql_client_cpp_test.cpp)。二者只链接通用库和测试框架。

```cmake
target_link_libraries(app PRIVATE TurboDB::MySQL)
```

在 VS 开发环境中运行仓库 preset：

```powershell
cmake --preset win-release-user
cmake --build --preset win-release-user --target turbodb_mysql mysql_client_test mysql_client_cpp_test
ctest --preset win-release-user -R "^mysql_client"
```

单元测试按 `tests/{session,wire,auth,parameters}` 分类，每类有独立 CMake。
测试使用已安装 SDK 的 `Salts::TinyTest`；会话故障测试还使用 `Salts::TinyMock`
替换测试编译单元中的 CNet 入口，验证初始化失败、连接拒绝、超时和资源释放，
不需要数据库服务。运行全部 MySQL 本地测试（含 ORM 适配器测试）：

```powershell
cmake --build --preset win-release-user
ctest --preset win-release-user -R mysql
```

库与公开头文件使用安装 component `MySQL`。私有协议探针和 E2E 故障注入头文件
不安装。原生查询通过 `mysql_session_open_prepared_source` 获取有界逐行 source，
写入通过 `mysql_session_execute_prepared` 获取行数与插入 ID；事务接口位于
`session_transaction.h`。配置错误在联网前返回 `MYSQL_SESSION_INVALID`，
详细阶段、服务端错误和传输状态通过 `mysql_session_error_t` 提供。

## 生命周期与边界

- 单线程、调用方驱动：调用者推进协议；同一 source/transaction 的操作不能并发。
- SQL、参数及连接配置只在打开/执行调用期间借用。调用返回后可释放输入。
- 非事务 source 拥有连接；事务 source 借用事务，必须先销毁 source，再提交、
  回滚或销毁事务。一个事务最多有一个活动 source。
- `next` 返回一行协议数据，行及错误消息借用到下一次 `next`、`cancel` 或
  `destroy`。列元数据借用到 source 销毁；需要长期持有时使用有界 row store。
- 列数、元数据字节、单行字节和命令字节由 cursor limits 限定。
  `max_result_rows` 是事务取消时的排空预算；直接消费原生 source 时，调用者
  自行控制总行数与累计字节配额。ORM 游标适配器继续执行既有总量限制。
  一次 `next` 只推进一行，不预先物化全部结果，不引入后台线程或无界队列。
- 打开失败不移交 source/transaction；调用者检查状态及 `mysql_session_error_t`。
  错误区分参数、协议、认证、I/O、超时和提交结果不确定；提交结果不确定时不重试。
- source 由其 `ops->destroy` 恰好释放一次。事务销毁前若仍活动，会尝试有界回滚。
  事务 source 取消按已有行预算排空，超出预算终止会话。
- TLS 始终验证证书及服务器身份，认证、容量和关闭状态机沿用原实现。

## 拆分决策与验证

此前根目录 MySQL 实际只生成 ORM MODULE，session 还使用 ORM 隔离级别和
错误码，无法独立消费（MED）。本次在原生边界定义 MySQL 状态与隔离级别，
由 ORM 适配器显式转换，保留 ORM 的公开错误语义和插件 ABI。

仅移动目录不能消除依赖；复制第二套客户端会产生两个协议事实源。选择单一
静态库供通用客户端和 ORM 插件共用，不改变算法、状态所有权或部署中的插件文件名。
静态链接不增加新的运行时 DLL，但会在各消费二进制中保留所需客户端代码。

先构建通用库，再构建插件与测试。回退时须同时恢复目录、CMake 和适配层的类型转换，
无需迁移数据库数据。验证包括只链接通用库的客户端测试、原有协议/认证/行测试、
ORM 错误转换与所有权测试，以及独立 E2E 构建。真实网络事务仍需配置测试数据库。
## 整段 schema 脚本

`session_script.h` 的 `mysql_session_execute_script` 接收连接配置、SQL 字节及长度、
调用方大小上限、语句计数输出和可选错误输出。它在一个独立 TLS 会话中发送整段 SQL，
按 MySQL 多结果标志读取所有 OK 响应；返回前关闭连接。SQL、配置仅借用到调用返回，
输出由调用方持有，单线程调用，无隐式重试。

大小同时受传入上限和 `MYSQL_SESSION_SCRIPT_MAX_BYTES` 约束；非法输入、分配或传输
失败、SQL 错误、结果类型不支持分别返回原生状态。失败计数为零，已提交语句不会撤销。
发送临时存储最多两个脚本大小的 buffer 加包头；响应使用已有固定控制 buffer，
不存在随语句数量增长的结果集合。处理时间为 O(SQL 字节数 + 响应字节数)。

此接口供 dbtools MySQL 插件复用；普通预处理语句不会启用多语句 capability。
使用示例、TLS 配置和事务限制见 [dbtools 文档](../dbtools/readme.md#mysql)，
协议与失败边界由 `mysql_script`、`dbtool_mysql_test` 和独立 MySQL E2E 覆盖。

## 异步结果源

`<turbodb_mysql.h>` 提供 `mysql_session_start_async_source` 和
`mysql_session_async_next`，通过调用线程上的 CNet `poll(0)` 推进网络。
WAIT 表示尚未就绪，调用方负责调度和总 deadline；METADATA 与 ROW 为借用视图。
关闭与超时所有权规则见 [ORM Stream / Async](../orm/stream-async.md)。
