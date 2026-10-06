# TidesSQL 执行引擎

2026-10-06 与 `master` 合并后的依赖契约：MySQL 客户端摘要使用 Salts API，
服务端密码验证继续直接使用 GmSSL；公开 SDK 不导出或附带 GmSSL 开发包。
PostgreSQL 使用无 SSL 的 libpq，TLS 由 CNet 提供。Plugin ABI epoch 跟随构建时
安装的 Salts SDK，宿主与模块必须匹配。下文分阶段记录中的 GmSSL SDK 打包、
libpq SSL DLL 和固定 Plugin ABI 4 描述是合并前的历史状态。

`sqlparser` 与 `tidessql` 是根目录下两个模块：前者提供 MySQL/SQLite 方言的
AST，后者负责语义绑定、表达式与查询执行、Catalog、关系记录、索引、事务预算
和原生 TidesDB 存储适配。解析器接受某种语法不表示执行引擎已经实现其语义；
当前执行入口仍按下文的 MySQL 方言 校验，不能用 SQLite AST 绕过它。
远程访问时 MySQL ORM driver 只渲染结构化 plan，MySQL client 只传输 COM_QUERY 或
COM_STMT_PREPARE/EXECUTE；SQL grammar 在 `tidessqld` 调用的 TidesSQL 引擎中解析。
直接链接本 SDK 时解析器与应用同进程，但所有权和语义事实源仍属于 TidesSQL 引擎。

构建 target 为 `turbodb_tidessql`，别名 `TurboDB::TidesSQL`。根选项
`TURBODB_BUILD_TIDESSQL` 默认跟随 `TURBODB_BUILD_ORM`；启用时必须启用
`TURBODB_BUILD_SQLPARSER`。引擎单元和原生集成测试由本模块的 `tests/` 加载，
不链接 ORM 动态库，也不加载插件。CTest 名字暂保留 `orm_tidesdb_sql_*`，
便于沿用既有过滤器；测试文件的实际归属已经迁出 ORM。

公开静态 C SDK 版本为 1.3.0，ABI v1，头文件为
[tidessql/tidessql.h](include/tidessql/tidessql.h)。引擎使用
[共享 DTO](../include/turbodb/types.h) 中的 `turbodb_value_t`、状态、错误和
option，不包含 ORM 头文件或链接 ORM runtime。ORM 保留旧名称、struct tag、
数值与布局作为兼容别名；内部 `orm_sql_*` 算法名称不属于 SDK，不安装。

连接选项解析、SQL 会话状态、自提交/显式事务、保存点和结果生命周期已迁入
[connection.c](src/connection.c)，由连接唯一拥有。公开 opaque 入口包括
`tdsql_connection_open/execute/query/begin/close`、
`tdsql_transaction_execute/query/savepoint/finish/release` 和
`tdsql_result_next/column/cancel/destroy`。输入是 SQL 文本和参数读取回调；
调用成功返回后查询保留自己的参数快照。结果、statement 和事务 handle 必须先释放，
否则连接关闭返回 BUSY；结果读到 EOF 或取消仍须 destroy。配置和请求必须
使用 `tdsql_config_default()` / `tdsql_request_default(sql)` 初始化；版本或
struct_size 不符返回 ABI_MISMATCH，在读取参数或访问存储前拒绝。

行是只读 `tdsql_row`，列元数据为 `tdsql_column`。参数及 SQL 借用本次调用，
结果值/列名借用至 next/cancel/destroy；固定消费者 context 随结果释放。
同一连接只允许一个同步 owner，回调不能重入、抛异常或 longjmp。
共享数据库模式新增 `tdsql_database_open/connect/close`：open 的配置、错误与
初始化规则沿用本地连接；connect 的数据库参数借用至返回，成功返回独立的
connection，失败清空输出；close 在存在任何会话时返回 BUSY 并保留数据库。
无会话时 close 消费数据库，包括原生 close 报错的情况；NULL 是成功空操作。
`sql_max_connections` 是正十进制整数，默认 128；到达容量返回 LIMIT_EXCEEDED，
不排队或扩容。连接释放后归还名额。数据库只在 open 初始化一次，不在 connect
重复初始化；配置字符串由数据库持有，各会话复制配置值并独立保存可变 SQL 状态。
同一共享数据库的全部操作必须由一个线程串行执行，包括会话与结果操作；
本版不提供跨线程并发 API。先释放结果/statement/事务，再 close 会话，最后 close 数据库。
原有 `tdsql_connection_open/close` 继续拥有私有数据库，保持既有用法和行为。
可执行的多会话示例与验收见
[database_test.c](tests/integration/database_test.c)。远程服务进度由
[#204](https://github.com/qigao/turbodb/issues/204) 跟踪；当前 SDK 增量对应
[#205](https://github.com/qigao/turbodb/issues/205)，尚无 MySQL 监听服务。
2026-10-05 本增量的完整构建及 95 项相关 CTest 全部通过（144.94 秒），
SDK 1.1.0 已通过 `install-win-release-user` 安装。直接共享数据库测试为
8 个用例；owner fault suite 为 49 个用例，包含新增的三个会话故障场景。
Linux/sanitizer 本轮未执行，仍需匹配平台与 Debug SDK。

SDK 1.2.0 新增 `tdsql_connection_run(connection, request, response, error)`：
request 沿用既有配置和 typed 参数契约，response 用 `tdsql_response_default()`
初始化。一次 AST 解析后分类执行，成功返回 `TDSQL_COMMAND` 的 affected_rows
或 `TDSQL_ROWS` 的 owned result；不会先试 query，再因错误尝试 command。
ROWS 的 result 仍须 checked destroy，释放上一结果后才能复用 response。
失败时完整 response 保持不变，但 SQL/清理失败仍可能要求事务回滚或隔离连接。
last_insert_id 为 0，因为执行器 不支持 AUTO_INCREMENT/LAST_INSERT_ID()；
显式主键不会被报告为自动生成 ID。现有 execute/query 继续可用，行为不变。

`tdsql_connection_state(connection, state, error)` 将事实源状态复制到
`tdsql_session_state_default()` 初始化的输出；无 SQL/分配/事务开启/诊断重置，
即使连接 BUSY 或隔离也可读取。in_transaction 包括 SQL/API 多语句事务，
不包括 autocommit 查询的临时快照；busy 表示结果/外部事务阻止 connection
执行。当前 transaction_read_only 与 session_read_only 分别取活动 owner 和
会话默认值；rollback_required/failure 必须单独检查。warning_count 为已产生
警告数，取最终状态前应完成结果消费和清理；EOF 本身不解除 BUSY。
输出版本/长度错误返回 ABI_MISMATCH，输出保持不变。只读不表示跨线程安全。
新 API 的正式使用与错误处理见 [dispatch_test.c](tests/integration/dispatch_test.c)
和 [C++ SDK 用例](tests/api/sdk_cpp_test.cpp)；使用新增入口需请求
`find_package(TidesSQL 1.2 CONFIG REQUIRED)`。
上述 1.2.0 增量由 [#206](https://github.com/qigao/turbodb/issues/206) 跟踪；该阶段的
公开 statement prepare、prepared 资源和 schema 变化处理尚未实现，当前入口见下文 1.3.0。
`tdsql_connection_prepare` 继续仅用于 ORM 适配器入场检查。
2026-10-05 本增量完整构建与 96 项相关 CTest 全部通过（145.08 秒），
SDK 1.2.0 已通过 `install-win-release-user` 安装。新增 dispatch suite 为
14 个用例、425 条断言；owner fault suite 增至 51 个用例，覆盖提交结果未知
及回滚失败后的纯状态观察。Linux/sanitizer 本轮未执行；网络服务与真正的
prepared statement 仍需后续实现和验收。

#206 的后续内部增量已拆出写语句纯绑定入口：INSERT/REPLACE VALUES/SET
（含 IGNORE、duplicate-key assignments/aliases）及单表 UPDATE/DELETE 可以仅用
AST、参数类型与现有 Catalog owner 检查语义，不传入参数值、不读取业务行或执行
表达式。绑定和执行共用目标列与表达式检查；正式验证见
[runtime_test.c](tests/integration/runtime_test.c) 的 `type-only write binding for #206`。
此首个写绑定增量不包含 INSERT SELECT；后续查询绑定已接入，见下文。未知参数
类型推导和 prepared handle 在该阶段尚未实现，查询/写依赖在后续增量接入；
私有头不安装，公开 SDK 与现有执行范围不变。边界及迁移见
[写语句纯绑定设计](design.md#206-写语句纯绑定边界)。
2026-10-05 该增量新增 11 个正式用例，runtime suite 共 240 用例全通过；
完整构建与 96 项相关 CTest 全通过（148.66 秒），正常安装 preset 已更新 SDK
1.2.0 静态库。Linux/sanitizer 本轮未执行。

#206 现已新增私有 `orm_sql_runtime_query_bind`：SELECT（含普通 JOIN、分组/窗口）、
集合运算和 query-group 可仅凭参数类型编译并取得列名/类型；INSERT SELECT 复用
此元数据匹配目标列后立即释放。不求值、不打开运行算子、不读取业务行或清空诊断。
query 拥有计划、列元数据及 Catalog 引用，AST/输入类型可在成功后释放；query 必须
在 owner/budget 结束前关闭，不能直接当成跨事务 prepared handle。next/cancel 在
显式开启执行前返回 INVALID_STATE。本阶段的查询依赖范围在下文继续扩展；
公开 SDK、原有执行范围和磁盘格式保持不变。正式验证见同一测试文件的
`type-only query metadata for #206`，所有权与边界见
[查询元数据绑定设计](design.md#206-查询列元数据与-insert-select-纯绑定)。
2026-10-05 本增量新增 13 个正式用例；runtime suite 共 253 用例、623684 条断言
通过。完整构建与 96 项 SDK/ORM CTest 全通过（170.24 秒），安装 preset 已更新
SDK 1.2.0 静态库。Linux/sanitizer 本轮未执行。

#206 后续新增独立的依赖图纯绑定模式：非递归 WITH、派生表、LATERAL 以及
scalar/IN/EXISTS 和相关查询可仅用参数类型取得列元数据。它复用词法 frame、
引用排序、分组捕获和源形状检查，不复制参数值、不构造 EXPLAIN 运行算子、
业务缓存或发布可求值业务源。AST/输入类型释放后元数据仍由 query 拥有；关闭顺序及配额
沿用既有 owner。带元数据依赖图的 query 不能开启执行，须 close；递归 CTE、
写语句 WITH/表达式子查询在下文接入，该阶段尚未接入公开 prepared 生命周期。正式用例见
[runtime_test.c](tests/integration/runtime_test.c) 的 `type-only dependency metadata for #206`；
详细所有权/范围见 [依赖无值绑定设计](design.md#206-查询依赖的无值绑定)。
2026-10-05 本增量新增 10 个正式用例；runtime 共 263 用例、631654 条断言全通过。
Windows Release 完整构建及 96 项 SDK/ORM CTest 全通过（166.82 秒），正常安装
preset 已更新 SDK 1.2.0 静态库。Linux/sanitizer 未执行，网络服务尚未实现。

#206 写语句纯绑定已接入非递归查询依赖：单表 UPDATE/DELETE 的 WITH、
scalar/IN/EXISTS、相关和嵌套 LATERAL 查询，以及 INSERT/REPLACE SELECT 的
WITH、派生/LATERAL 表和表达式依赖。只校验 Catalog/类型/捕获，不传入参数值、
求值、读写业务行、创建保存点或改变警告；返回前关闭全部临时依赖。
递归准备、INSERT VALUES/SET/duplicate assignment 的查询表达式仍明确拒绝，
现有执行范围保持不变。验证见 [runtime_test.c](tests/integration/runtime_test.c)
的 `type-only write dependencies for #206`，所有权与迁移见
[写依赖无值绑定设计](design.md#206-写语句查询依赖的无值绑定)。
该阶段尚未实现未知参数推导及公开 prepared 生命周期；无 MySQL 监听服务。
2026-10-05 本增量新增 10 个正式用例；runtime 共 273 用例、640024 条断言
全通过，Windows Release 完整构建及 96 项 SDK/ORM CTest 全通过（170.76 秒），
安装 preset 已更新 SDK 1.2.0 静态库。Linux/sanitizer 未执行；代码未提交/推送。

#206 新增私有标量参数推导器，显式区分 unresolved 与实际 NULL：根据 local/outer schema、
同级操作数、赋值上下文或 CAST 目标推导 source-order 参数类型，全部完成后才
提供只读视图，再交给既有 typed Binder 校验。推导只编译、不求值或读取业务行；
AST 释放后完整类型视图仍由参数 owner 持有，须在 statement budget 结束前 close。
当前支持标量算术/二元比较、scalar-list IN/NOT IN、BETWEEN/NOT BETWEEN、CASE、
LIKE、NOT/AND/OR、numeric CAST，以及 COALESCE/IFNULL/NULLIF 和现有数值标量函数；
其他函数及查询依赖的未知参数推导明确拒绝，已有
显式 typed 执行范围不变。
这是标量内部构件；常用整句编排和公开 prepared 生命周期见下方增量。
正式例子见 [parameters_test.c](tests/unit/parameters_test.c) 和原生
[runtime_test.c](tests/integration/runtime_test.c)，边界见
[未知参数推导设计](design.md#206-未知标量参数类型推导)。SDK/ABI 和磁盘格式不变。
2026-10-05 新增推导 suite 的 15 用例/1274 断言全通过；原生 runtime 新增
1 用例后，共 274 用例/640112 断言全通过。完整构建及 97 项 SDK/ORM CTest
全通过（170.99 秒），安装 preset 已更新 SDK 1.2.0 静态库。runtime 原生 suite
的测试 TIMEOUT 已由 60 秒调整至 90 秒，生产配置不变。Linux/sanitizer 未执行，
代码未提交/推送。

#206 已接入私有 `orm_sql_parameters_statement`：仅凭 MySQL AST 和当前 Catalog
schema，自动遍历常用 SELECT、INSERT/REPLACE VALUES/SET、单表 UPDATE/DELETE，
推导赋值、比较、排序、CAST 与 LIMIT/OFFSET 的 source-order 参数类型，最后
通过既有整句 Binder 校验。成功只保留参数元数据，AST 可释放，临时计划/Catalog
租约均关闭；不接收实际值、不求值、读写业务行或改变事务/诊断。
普通表 JOIN 的 ON、可独立标量定型的 GROUP BY/HAVING、聚合参数，以及窗口
key/control/frame marker 已按真实多表 schema 和最终 Binder 推导。非递归 CTE、
derived、标量/IN/EXISTS 子查询会按依赖拓扑逐 query block 推导：子块先用自己的
FROM 与只读 outer frame 定型，再发布 schema 给父块；未解析位图阻止其他 query block
的零初始化 type storage 被消费。scalar dependency result 与 IN element type 可向父表达式
传播；compound 会先绑定投影中没有未决 marker 的叶子，再把对应输出列类型传播给其余
叶子的投影 marker，左右分支方向、数值 DOUBLE 提升及 LIMIT/OFFSET 均可推导。
需要 INSERT SELECT/incoming alias，以及未列入白名单的
函数表达式仍明确拒绝；
scalar-list IN、BETWEEN、CASE、LIKE、NOT/AND/OR、
COALESCE/IFNULL/NULLIF 与数值标量函数使用标量推导器，既有显式 typed 执行不变。
该内部构件不是公开 prepared handle，不支持跨事务计划、RESET/CLOSE 或 schema 重准备。
范围/所有权见 [整句参数推导设计](design.md#206-常用整句参数推导)，正式例子见
[runtime_test.c](tests/integration/runtime_test.c) 的 `whole statement unknown parameter inference`。
2026-10-05 新增 9 用例，最终 runtime 283 用例/647352 断言、标量 suite
15 用例/1274 断言全部通过。完整构建及 97 项 SDK/ORM CTest 全通过（180.22 秒），
补充异型列覆盖后的两项最小回归通过（61.20 秒）；安装 preset 已更新 SDK 1.2.0。
公开 API/ABI、配置、依赖和磁盘格式不变；Linux/sanitizer 未执行，代码未提交/推送。

SDK 1.3.0 已增加真正的公开 `tdsql_connection_statement_prepare()` 与
`tdsql_statement_parameters/parameter/columns/column/execute/reset/close()`。
PREPARE 接收无参数值的 request（parameter_count 为 0，reader/context 为 NULL），
独立保存 SQL、初始参数/列元数据以及每个物理 Catalog 来源 occurrence 的表 ID/schema；
CTE 引用按既有词法绑定结果跳过，定义、派生表和表达式子查询下的实体表仍纳入指纹。
准备不求值、读写业务行、重置警告
或消耗 next transaction access。已有 SQL 事务用其快照，其他情况关闭临时 Catalog
事务；statement 不保留 native transaction/iterator/物理计划。旧的
`tdsql_connection_prepare()` 仍只负责 ORM 入场检查。

EXECUTE 接收 `tdsql_bindings_default(values,count)` 初始化的 typed bindings，
参数数量须完全匹配；七种值和字节 payload 沿用原有执行契约，不拼接 SQL。
每轮执行在当前快照中先检查全部已记录来源的 table ID/规范 schema，再重新绑定并执行。
普通 DML 的 TableVersion 变化不使描述符失效；任一来源删除/重建或修改列/默认值后返回 INVALID_STATE
并锁定失效，须 close 后重新 prepare，RESET 不恢复。当前没有
[MySQL 的自动重准备](https://dev.mysql.com/doc/refman/8.4/en/statement-caching.html)。
初始列类型是推导结果，每轮 `tdsql_result_column()` 才是实际执行元数据；未来
frontend 须发送完整 metadata，不协商 optional result metadata。

公开 PREPARE 范围为 SELECT 无 FROM/普通表 JOIN（含上述可推导的 GROUP/HAVING 与窗口子集）、
INSERT/REPLACE VALUES/SET、
单表 UPDATE/DELETE、无 marker 或 WHERE 使用当前标量推导子集的 SHOW，以及同一
普通表/JOIN 推导子集的 EXPLAIN SELECT；
含 marker 的算术/比较/scalar-list IN/BETWEEN/CASE/LIKE/NOT/AND/OR/numeric CAST、
COALESCE/IFNULL/NULLIF 及 ABS/SIGN/FLOOR/CEIL/CEILING/MOD/ROUND/TRUNCATE 可推导，
非递归 CTE、derived 及 scalar/IN/EXISTS 依赖在同一拓扑中推导并生成来源指纹；
scalar result、IN element 及 compound 兄弟分支输出支持父级 marker 推导；显式配置
正数 `sql_max_recursive_iterations` 时，有界递归 SELECT 的 seed/member 也可无值推导；
其余函数、递归 UPDATE/DELETE 查询依赖、
INSERT SELECT/incoming aliases、SHOW LIKE 子句 marker/含未支持表达式的 WHERE 明确返回
UNSUPPORTED。已有显式 typed run
支持范围保持不变。
持久化表列仍限 BIGINT/BIGINT UNSIGNED/DOUBLE，不等于参数值只支持这三类。

SHOW/EXPLAIN 准备只在当前 Catalog snapshot 中构造输出 metadata，传入真实 column
family 显示名和 connection session snapshot。带 WHERE 的 SHOW 以显示 schema 推导并
编译 predicate，但不创建 scan 或读取行；取得列名/类型后立即关闭临时 query、依赖图和
Catalog lease。SHOW/EXPLAIN descriptor 仍只持有 SQL、参数类型与复制后的列 metadata，
不缓存 AST/计划；查询 descriptor 另保留根查询及全部依赖查询中每个物理 Catalog 来源的
identity/canonical schema slice，任一来源变化都会锁定失效。两类 metadata 语句的输出形状
固定，EXECUTE 重新解析并绑定当前 snapshot。

DDL PREPARE 已接入当前引擎支持的 CREATE/ALTER/DROP/TRUNCATE TABLE 与
CREATE/DROP INDEX。没有参数或结果列；准备只校验结构和必要的 Catalog/索引
目录元数据。DEFAULT 编译而不求值，除零、转换、范围、NOT NULL 默认值和非空
表重写检查在 EXECUTE；不读取业务行或物理索引项，不写入、不分配 ID 或升级
Manifest。DDL descriptor 保存 SQL，每次 EXECUTE 重新绑定当前元数据，成功
修改目录不会使自身锁死；普通重复操作依照 CREATE/ALTER/DROP 的实际错误和
IF EXISTS/IF NOT EXISTS 意图返回。CREATE 名称冲突与索引唯一性/物理容量仍在
执行检查。事务回滚与 READ ONLY 行为保持，不采用 MySQL DDL 隐式提交。
范围依据：[MySQL prepared statements](https://dev.mysql.com/doc/refman/8.4/en/sql-prepared-statements.html)，
边界设计见 [DDL PREPARE](design.md#206-ddl-prepare-边界)。

每会话 `sql_max_prepared_statements` 默认 64，`sql_max_prepared_bytes` 默认
16 MiB，均须为正十进制整数；配额包含对象、向量 metadata/alignment、SQL、schema、
参数和列名，单 handle 同时受原有 WORK 限额。成功才占名额，失败退款，close 释放。
SQL/元数据由 handle 拥有；bindings 借用本次 execute，查询复制 retained payload。
result 活着时（含 EOF/cancel）该 handle 的 execute/reset/close 返回 BUSY；必须先
checked destroy。RESET 验证 owner/失效/空闲状态并保留 SQL/metadata，不修改事务
或警告；不保留绑定值，不支持 long-data/server cursor。close 消费 handle，即使清理
报错也不可再使用，并隔离连接。连接 close 在仍有 statement 时 BUSY。
现有 ABI v1 DTO 布局、磁盘格式与依赖不变；使用新增入口需
`find_package(TidesSQL 1.3 CONFIG REQUIRED)`。完整调用例子见
[prepared_test.c](tests/integration/prepared_test.c)、
[C++ SDK 用例](tests/api/sdk_cpp_test.cpp)，失败/配额/native 清理契约见
[prepared_owner_test.c](tests/integration/prepared_owner_test.c)，设计见
[prepared 生命周期](design.md#206-sdk-prepared-生命周期设计)。

事实｜2026-10-05 最新 Windows Release 的 30 项 TidesSQL CTest 全部通过
（125.11 秒），包括 prepared 的 28 用例/3277 断言和 prepared owner 的
12 用例/433 断言；runtime 283 用例/647352 断言保持通过。上一阶段中间一次最小回归
在 TinyTest 创建临时目录时返回 NULL，SQL 尚未调用；随后完整重跑通过。
首次测试修正了只读写入/非法 TEXT 的既有错误码预期和无别名列的构造；生产执行
规则未改。另修复 schema 校验失败被 context-size 错误覆盖，以及 Catalog begin
读失败后同码回滚失败无法被 SDK 区分的问题，均有正式回归。
DDL 增量新增 12 个用例，覆盖全部现有分支、默认值检查阶段、重复执行、事务
回滚/只读、逐点 WORK 分配失败及 native 读写/提交/回滚观察；业务命名空间
读取探针开启拒绝时仍能准备，physical row/index 的 get/seek 均为 0。

事实｜2026-10-06 SHOW/simple EXPLAIN、scalar-list IN/BETWEEN/CASE/标量函数，以及
普通 JOIN/group/window 参数推导与多来源 schema 失效增量后，prepared suite 为
33 用例/11483 断言，MySQL dispatch suite 为 18 用例/2120 断言，参数推导 suite
保持 21 用例/3188 断言，runtime suite 为 284 用例/647468 断言，均无失败、跳过或
TODO；相邻 prepared owner、registry、registry owner 与 dispatch 四项 CTest 也全部
通过。覆盖 SDK 和 COM_STMT_PREPARE/EXECUTE 的列名、参数化 SHOW/EXPLAIN、二进制行、
TABLES/TEXT、IN/BETWEEN/CASE/LIKE、选择/数值标量函数与布尔参数、INDEX/I64、
VARIABLES/session snapshot、dependent EXPLAIN/未知函数拒绝、descriptor 原子发布、
多来源 schema 失效及复合比较/LIKE/逻辑/函数/JOIN 路径的逐点 WORK reserve/resize
退款。递归准备、Linux/sanitizer 与外部 Connector 验证仍属
#206/#209。

事实｜2026-10-06 私有整句推导继续覆盖非递归 CTE、派生表、scalar/IN/EXISTS
依赖查询及相关外层 schema；未解析 marker 由显式 resolved mask 隔离，不再把零初始化
存储当成类型。durable prepared 复用 CTE lexical binder，跳过 CTE 引用并为依赖图下的
物理 Catalog 来源保存 identity/canonical schema slice；SDK 与 COM_STMT_PREPARE/EXECUTE
现可执行 marker-bearing CTE、derived、scalar、IN、EXISTS 及 dependent EXPLAIN，来源
schema 改变仍在参数读取/业务执行前锁定失效。参数推导 suite 为 23 用例/3216 断言，
runtime suite 为 288 用例/653295 断言，prepared suite 为 33 用例/16414 断言，MySQL
dispatch suite 为 19 用例/2222 断言；完整 Windows Release 构建及 prepared/owner、
registry/owner、dispatch、select、from、parameters、runtime 共 9 项相邻 CTest 顺序执行
全部通过（74.13 秒）。跨 compound 分支输出传播已在后续增量补齐；递归推导及
Linux/sanitizer 仍属后续。

事实｜2026-10-06 后续增量已接入 scalar query result、IN query element 与可独立定型的
compound 分支/尾部分页参数传播；公开 SDK 及 MySQL COM_STMT_PREPARE/EXECUTE 均验证实际
结果，prepare allocation fault matrix 覆盖新增路径。参数 suite 为 23 用例/3222 断言、
runtime 为 288 用例/653316 断言、prepared 为 34 用例/22843 断言、dispatch 为 20 用例/
2414 断言；四项 Windows Release CTest 顺序执行全部通过（65.01 秒）。后续增量已补齐
跨 compound 分支输出类型传播；递归准备、Linux/sanitizer 与外部 Connector 仍属后续。

事实｜2026-10-06 compound PREPARE 会把 marker-free 叶子的真实 SELECT 输出类型作为
只读上下文，按投影序号传播给有未决 marker 的兄弟叶子；已覆盖 marker 位于左右任一
分支、I64 列传播、三分支 DOUBLE 提升及全局 LIMIT。上下文只在对应叶根绑定期间借用，
不进入嵌套依赖；临时类型向量受 WORK 预算约束并在成功或失败时退款。SDK 筛选用例
1 项/62 断言、MySQL binary protocol 筛选用例 1 项/281 断言通过；prepared、dispatch、
runtime 三项 Windows Release CTest 全部通过（65.28 秒）。递归 PREPARE、Linux/sanitizer
与外部 Connector 验证仍属后续。

事实｜2026-10-06 有界递归 SELECT 已接入公开 PREPARE 与 MySQL binary protocol。准备阶段
只编译 seed/member schema 和参数类型，不传参数值、不打开递归 cache、不迭代或读取业务行；
执行仍使用同一 `sql_max_recursive_iterations` 事实源。未配置正数上限时保持 UNSUPPORTED，
递归 UPDATE/DELETE 查询依赖仍不在本增量范围。SDK/runtime/MySQL 三项递归筛选用例分别
通过 49/49、49/49、396/396 条断言；全量 prepared 36 项/33378 条、runtime 289 项/
653365 条、dispatch 20 项/2618 条断言全部通过，三项 Windows Release CTest 顺序执行
耗时 65.79 秒。Linux/sanitizer 与外部 Connector 验证仍属后续。

后续已将 ORM Driver SDK 与全部插件统一升级至当前 Salts Plugin ABI 4，并完成
真实 MySQL Driver 到独立 `tidessqld` 的 TLS/认证/事务链路验证。ABI 3 插件必须与
宿主协调重编译，不能混部署；该升级不改变 TidesSQL SDK ABI、MySQL wire 或磁盘格式。
Linux 实际 runner 与外部 MySQL 服务端差分仍由独立环境验证；Windows sanitizer 结果见下文。
最小复验（VS x64 开发环境）：

```powershell
cmake --build --preset win-release-user --target tidessql_prepared_test tidessql_prepared_owner_test tidessql_sdk_cpp_test -j 4
ctest --preset win-release-user -R '^tidessql_(prepared|prepared_owner|sdk_cpp)$' -j 2
```

完整引擎日志在 `build/Msvc-Release/Testing/prepared-ddl-engine-regression.log`；
测试细节为 CTest 的 `Testing/Temporary/LastTest.log`。

#207 已新增私有 [MySQL server codec](server/mysql/wire.h)：有界消息分帧/组帧、
序号与粘包处理，QUERY/PREPARE/EXECUTE/CLOSE/RESET/PING/QUIT 命令解码，原生
参数类型缓存，以及 OK/ERR/EOF、prepare OK、列元数据和文本/二进制行编码。
复用现有 `mysql/wire` 基础源码；不分配内存、不执行 SQL。完整请求校验后才
发布参数/类型，编码失败不改发送区；TEXT/BLOB 借用至本次请求结束。
signed TINY 0/1 沿用现有 driver 的 BOOL 映射，其他整数宽度与 FLOAT 原生解码；
不支持的命令/游标/query attributes 明确拒绝。边界见
[codec 设计](design.md#207-mysql-服务端-codec-边界)。

事实｜2026-10-05 新协议 [suite](tests/protocol/mysql_wire_test.c) 的 24 用例、
734 条断言全部通过，包含独立 golden、现有 client encoder/decoder 互操作、
真实 0xffffff 分片/空终止包、序号回绕、截断/容量/算术溢出和输出原子性。
Windows Release 的全部 31 项 TidesSQL 与 7 项 MySQL client CTest 均通过
（76.70 秒），本轮构建无编译警告或错误。日志为
`build/Msvc-Release/Testing/mysql-codec-engine-regression.log`。最小复验：

```powershell
cmake --build --preset win-release-user --target tidessql_mysql_wire_test mysql_packet_boundary_test -j 4
ctest --preset win-release-user -R '^(tidessql_mysql_wire|mysql_packet_boundary)$' --output-on-failure
```

MED｜codec 不安装、不开放监听器；后续 registry/响应层进展见下，握手/能力协商
仍待 #207，TLS/认证/调度与真实远程 driver E2E 分属 #208/#209。
此 codec 测试未验证网络、多语句的 frontend 拒绝或复杂 PREPARE。既有 Salts/ORM
Plugin ABI 阻塞仍存在，本轮没有 ORM 全量回归或安装更新。未运行 Linux、
sanitizer 或 benchmark；代码未提交/推送，#204/#207 保持开放。

#207 后续已新增私有 [registry](server/mysql/registry.h) 与
`tidessql_mysql_frontend` target。每个 SQL connection 使用固定 CSTL Vec slots，
MySQL ID 仅对应本连接的真实 SDK handle；成功 PREPARE 后才发布递增 ID/counts，
关闭后不复用 ID，uint32 耗尽明确拒绝。SQL/schema/names 仍由 SDK 持有。原生
EXECUTE 复用 wire type cache，直接传 typed bindings，不拼接 SQL；返回后临时
values 清零，结果拥有自己的参数副本。RESET 保留 cache/ID/事务；CLOSE 未知
ID 按官方协议无响应处理，BUSY 保留映射，其他 cleanup error 消费并锁定 owner。
关闭顺序为 checked destroy 结果 → dispose registry → close SQL connection，
最后一步执行断连事务回滚。设计与容量模型见
[registry 边界](design.md#207-statement-registry-与-sdk-接入)。

事实｜2026-10-05 [registry suite](tests/protocol/mysql_registry_test.c) 的
16 用例/647 断言与 [native owner suite](tests/protocol/mysql_registry_owner_test.c)
的 3 用例/92 断言全部通过；真实 SDK/原生存储覆盖 DDL/DML、结果字节所有权、
缓存/预算/分配失败、连接 ID 隔离、schema 失效及断连回滚。native commit 确认
丢失返回 COMMIT_UNKNOWN，独立会话验证已提交，重复 EXECUTE 未重放 commit。
多语句准备被既有单语句 parser 限额拒绝，业务数据与 ID 未推进。
全部 33 项 TidesSQL + 7 项 MySQL client CTest 通过（104.88 秒），本轮构建无
编译警告/错误；日志 `build/Msvc-Release/Testing/mysql-registry-engine-regression.log`。

```powershell
cmake --build --preset win-release-user --target tidessql_mysql_registry_test tidessql_mysql_registry_owner_test -j 4
ctest --preset win-release-user -R '^tidessql_mysql_registry(_owner)?$' --output-on-failure
```

#207 已接入私有 [command dispatcher](server/mysql/dispatch.h)，QUERY/PING/QUIT
与 prepared 命令直接调用 SDK，返回完整 prepare/text/binary 响应链。实际 SDK
metadata/affected rows/事务 flags/warnings 是事实源，两种 EOF 模式显式选择。
固定 scratch 每次持有一个包，ACK 后推进；frame 容量不足保留原包，重取不会重读
行或重放提交。checked destroy 结果后才编码结束状态；清理失败/提交未知发送
ERR 后断连。断连先释放结果/registry，再 close SQL session 执行回滚。
[响应设计与边界](design.md#207-command-响应状态机)。

事实｜2026-10-06 [dispatch suite](tests/protocol/mysql_dispatch_test.c) 17 用例/
1871 断言、扩展 native owner suite 6 用例/213 断言全部通过。覆盖两种 EOF、
空集/多行、动态 prepared metadata、七种值、DDL/DML、RESET cache、错误与
多语句 admission、ACK/容量重取、lazy warnings/事务 flags、SDK/scratch/row
限额、SHOW/参数化 EXPLAIN 的 prepared metadata/二进制行及断连回滚。native 故障验证末尾清理失败 ERR、提交未知不重放与 PREPARE
snapshot cleanup 不发布 metadata。最小 2 项 CTest 通过（4.21 秒），构建无
编译警告/错误；日志 `build/Msvc-Release/Testing/mysql-dispatch-final-test.log`。
Windows Release 的全部 34 项 TidesSQL 与 7 项 MySQL client CTest 通过（82.57 秒），
日志 `build/Msvc-Release/Testing/mysql-dispatch-engine-regression.log`。

```powershell
cmake --build --preset win-release-user --target tidessql_mysql_dispatch_test tidessql_mysql_registry_owner_test -j 4
ctest --preset win-release-user -R '^tidessql_mysql_(dispatch|registry_owner)$' --output-on-failure
```

#207 已有私有 [handshake codec 与 negotiation gate](server/mysql/handshake.h)：
greeting 只声明已实现的协议形状，SSLRequest 后等待实际 transport TLS ready，
再校验 login 与原 header 一致。纯协议成功仍等待账户验证，不创建 SQL session。
用户名/数据库/token views 不跨输入 release；额外能力、截断、非零 filler、
header 变更和错误顺序明确拒绝。此 gate 不承担账户认证或创建监听服务；
后续密码原语与真实 TLS 验证进展见下。
[握手边界与验证](design.md#207-tls-前后握手边界)。

事实｜2026-10-05 handshake suite 14 用例/659 断言通过；完整 35 项 TidesSQL
与 7 项 MySQL client CTest 全通过（75.73 秒），本轮构建无编译警告/错误。
日志 `build/Msvc-Release/Testing/mysql-handshake-engine-regression.log`。

```powershell
cmake --build --preset win-release-user --target tidessql_mysql_handshake_test -j 4
ctest --preset win-release-user -R '^tidessql_mysql_handshake$' --output-on-failure
```

#208 已接入私有 [密码验证边界](server/mysql/password.h)：TLS full-auth 请求、
GmSSL 随机 salt/PBKDF2-HMAC-SHA256 verifier 与恒定时间比较，拒绝空密码、
嵌入 NUL、超限和 TLS 前输入；库失败保持 caller output 并清零临时秘密。
MySQL client 的 token 哈希已改用 GmSSL，协议格式不变，vcpkg/CMake 及安装规则
同步迁移，GmSSL release/debug archive 随生产者 package 提供。
[依赖、所有权与兼容性设计](design.md#208-gmssl-与密码验证边界)。

事实｜2026-10-05 新密码 suite 10 用例/76 断言、真实 TCP/CNet TLS suite
4 用例/264 断言及 client authentication suite 5 用例/26 断言通过。
TLS 测试实际导入 Salts CNet，完成同一连接上的 server/client TLS 1.3 升级、
加密 login/full-auth payload 收发和密码校验；错误身份、独立错误 CA 及握手
停滞拒绝。夹具的有效期符合 GmSSL 的上限，测试未关闭证书/身份验证。
测试使用两端 CNet 与私有 codec/verifier 的显式编排，尚未通过 MySQL packet
framing 连接完整服务或使用现有 MySQL session/ORM driver；不能视为 #209 E2E。
Windows Release 37 项 TidesSQL + 7 项 client CTest 全通过（72.63 秒），
构建无编译警告/错误。日志 `build/Msvc-Release/Testing/mysql-gmssl-engine-regression.log`。

私有 [账户授权 owner](server/mysql/authenticate.h) 将精确用户名映射到显式逻辑
数据库 grant；服务控制面持有的数据库 binding 直接引用已打开的共享数据库，
客户端名称不会进入文件路径 API。未知账户仍执行同一工作因子的完整 KDF 后统一
拒绝；密码成功后才创建 SDK session，最终认证 OK 完整交接后才可 move 给 command
owner。失败/断连关闭未移交 session；BUSY cleanup 可重试，其他 cleanup failure
隔离 owner。未知/未授权数据库、session quota、OOM 和内部失败映射为明确 MySQL
ERR；错误消息不含密码、verifier 或服务端路径。

事实｜账户 suite 27 用例/1223 断言通过，使用两个真实本地 TidesDB 和实际 SDK
session/dispatcher，覆盖精确 grant、默认数据库、路径样式名称拒绝、资源限额、
故障与清理。合并本增量后 Windows Release 38 项 TidesSQL + 7 项 MySQL client
CTest 全通过（83.63 秒），日志 `build/Msvc-Release/Testing/mysql-auth-engine-regression.log`。
私有 [CNet server adapter](server/mysql/server.h) 现已把 framed frontend 接到真实
listener：固定 slot/connection workspace、单次在途 retained send、同 handle TLS upgrade、
终态 session 回滚与 cleanup quarantine 均由单 owner poll 推进。它仍不安装，也没有
daemon CLI/TOML 或部署账户文件。

事实｜正式 `tidessql_mysql_server_e2e` 现有 8 个用例；最新成功运行 358 条断言全部通过。仓库现有 `TurboDB::MySQL`
async session 通过 loopback TCP 和证书校验 TLS 完成 full-auth、prepared
`SELECT 42 AS n`、metadata/row 与关闭；另测错误密码、正确密码但无数据库 grant、
两个同时在线 session、TLS 前停滞客户端的有界停止和 read timeout 主动回收、零超时 stop
的 BUSY/重试状态机，以及无界/畸形服务配置的原子拒绝。慢客户端用例在 client 被接受后
停止推进其异步状态机，仅轮询 server，确认 CNet 发布 `SALTS_ETIMEDOUT`、slot/workspace
回收且 transport failure 可观测；完整目标连续运行 5 次通过，该慢客户端用例随后隔离连续
运行 10 次也全部通过。
双 session 用例先让第一连接进入 metadata，再在其结果会话仍存活时建立并执行第二连接；
它验证同时在线 slot/结果隔离，不把两个提供者握手重叠当作 TidesSQL 状态机契约。

HIGH｜历史诊断：Windows 重复压力测试曾发现 GmSSL TLS 1.3 `CertificateVerify` 间歇失败；
同一签名校验栈也在单客户端 transaction E2E 和独立 `tidessqld` ORM E2E 中复现，因此不能
归因于双会话 slot。影响是远程连接可用性和测试稳定性，不是已观察到的 SQL 数据错误。
事实｜当时的 Salts SDK 1.8.25（commit `21f02db32def5a3a4f8d28f97abed0f9aaef8090`）
在 CNet 中静态链接 GmSSL #5，基于上游 `7c9f02904ef33e59c87b4f16621cc8fd434e7579`。
该版本的 `ecdsa_signature_from_der` 允许 ASN.1 INTEGER 去除前导零后长度小于 32，随后却把
变长地址直接交给固定读取 32 字节的 `secp256r1_from_32bytes`；31 字节的 `r` 或 `s` 会把
相邻 DER 内容读入椭圆曲线整数。[上游修复 c5ee40e3](https://github.com/guanzhi/GmSSL/commit/c5ee40e3dfb640547afea7f3a9fc8fe0c9412d4a)
通过清零 32 字节缓冲区并右对齐复制 `r/s` 修正这一边界。

事实｜未打补丁的旧 CNet 完整 server suite 在第 8 次重复运行命中同一校验失败；以该
上游修复回移到实际旧版 API、重新构建 GmSSL #6 和 Salts/CNet 后，完整
`tidessql_mysql_server_e2e`、`tidessql_mysql_server_transaction_e2e`、
`orm_mysql_tidessqld_e2e` 分别连续 20/20 通过，共执行 280 个正式测试用例。测试未增加
重试、串行 accept 或协议降级。另以合法的 31 字节 `r` 构造确定性 DER 契约，旧 provider
编译后运行失败，补丁版同一源码通过。

事实｜永久发布链已于 2026-10-06 完成：`qigao/vcpkg-cache` PR #182 发布带上述确定性契约的
GmSSL #6（merge `b8bcce436838b716522bb2c8b35438b47ac1830b`），PR #185 对齐 Salts manifest/
registry 身份与首次安装顺序（merge `ab71347f4df19381c819ce8989cb22cb3c61da10`）；cache-only
验证从 NuGet 恢复 ABI `589630979a67bf8bd74bd84176635e9fc518c9bd1800f1f9fa3027a49de71fbe`。
Salts `v1.8.27` 精确指向 commit `c11b508cc6c4d54a34404a9849bd89023d1790a3`，release workflow
run `37399469140` 的七个平台 SDK、Linux arm64 cache-only/smoke、打包和发布全部通过。
发布资产 `Salts.Native.1.8.27.nupkg` 的 SHA-256 为
`670cf8fdcb97d188935ae1e00fb00a14c5bddd56b26d81390affc039747729e7`。
TurboDB 已用该发布资产的 Windows x64 Release SDK 全量重建；TLS、server E2E、transaction
E2E 各连续 20/20 通过（共 60 次进程测试，293.53 秒），没有重试或协议降级。该 HIGH 发布
阻断已关闭。相邻 MySQL frontend/daemon/ORM 远程组合 15/15 通过（53.15 秒），最终 Windows
Release 全套 142/142 通过（433.72 秒）。全套首次运行暴露 relational 集成测试仍按旧的 3 项
SHOW 变量和 `@@sql_mode` 不支持语义断言；对齐当前 23 项只读变量契约后，该 suite 292/292、
16580 条断言通过，生产实现未改。有限重复次数仍不能证明所有 TLS 路径不存在其他独立问题。

现有同步 `TurboDB::MySQL` transaction API 也已通过真实 server E2E。测试以一个有界
Salts Executor shard 持有 server poll owner，客户端经 TCP/TLS/full-auth 完成远程
DDL、参数化 DML、SERIALIZABLE BEGIN/COMMIT/ROLLBACK、SAVEPOINT/ROLLBACK TO/RELEASE，
并由新连接验证事务内 DDL 和行状态。显式回滚及销毁活动 transaction 均撤销未提交更新。
格式正确的 MySQL ERR_Packet 现返回 `MYSQL_SESSION_SQL_ERROR` 和结构化 code/SQLSTATE；
损坏 ERR 才返回 `MYSQL_SESSION_PROTOCOL`，服务端文本仍不对外复制。
另一个真实 prepared result 覆盖 NULL、两类整数边界、DOUBLE、BOOL、TEXT、BLOB、
列类型/unsigned flag、二进制行值和 affected rows。

事实｜`tidessql_mysql_server_transaction_e2e` 5 个用例/192 条断言通过；其中断连用例在
远程未提交更新后停止服务，并由本地 SDK 验证 server cleanup 已回滚该更新。TidesSQL
MySQL client 的测试私有 fault hook 另在 COMMIT 完成 CNet 发送后丢弃确认：返回
`MYSQL_SESSION_COMMIT_UNKNOWN`、发送计数为 1，server 清理后本地 SDK 观察存储值只能是
提交前值或一次提交后的值。新增真实会话先以 COM_QUERY 执行 `SET autocommit=OFF`，
再以 PREPARE/EXECUTE 写入：直接断开回滚该行，切回 ON 则提交并可由新连接读取。
生产 client target 不包含这些测试 hook。MySQL protocol、client 与 daemon 相邻回归
20/20 通过（41.98 秒），复验日志为
`build/Msvc-Release/Testing/tidessql-autocommit-regression.log`。通用 Connector 初始化 SQL
已由后述 Connector/J 进程 E2E 覆盖；剩余远程缺口包括 Linux 实际 runner。

```powershell
cmake --build --preset win-release-user --target tidessql_mysql_password_test tidessql_mysql_tls_test mysql_auth_boundary_test -j 4
ctest --preset win-release-user -R '^(tidessql_mysql_(password|tls)|mysql_auth_boundary)$' --output-on-failure
```

可部署 `tidessqld` 已作为默认关闭的可选目标接入：配置时启用
`TURBODB_BUILD_TIDESSQL_SERVER=ON`，随后构建/安装 `tidessqld`；仓库标准 Windows/Linux
user 与 CI preset 已选择该选项，裸 CMake 默认仍为 `OFF`，Android 与外部数据库 E2E
profile 不构建 daemon。它只接受显式绝对
TOML 路径，严格验证 numeric host、TLS keypair、CNet capacity、database/account/grant
以及 PBKDF2 verifier，再按 database → auth policy → listener 顺序启动。完整配置和边界见
[部署入口](design.md#204208-tidessqld-部署入口)。

```powershell
tidessqld hash-password --password-env TIDESSQLD_PASSWORD
tidessqld check-config --config C:/etc/tidessql/tidessqld.toml
tidessqld serve --config C:/etc/tidessql/tidessqld.toml
```

事实｜`tidessqld_config` 10 项/220 条断言、`tidessqld_cli` 5 项/45 条断言和
`tidessqld_process_e2e` 1 项/16 条断言通过。
配置 suite 覆盖恰好 1 MiB、1 MiB+1、嵌入 NUL，以及多数据库启动中途失败后的逆序关闭；
server 数值字段的零值/精确最小值、queue 形状、frontend scratch 和 server version 边界
与实际 CNet/MySQL frontend 准入一致；
CLI suite 以有界独立子进程覆盖 help/version、配置检查、严格参数拒绝、PBKDF2 输出形状、
空密码环境及非法迭代次数，并确认输出不包含输入明文；进程 E2E 启动独立 daemon，由现有 `TurboDB::MySQL` 经 TLS/full-auth 完成事务内 DDL、DML、
SAVEPOINT、COMMIT 与 prepared SELECT。Windows install component 同时安装 executable、
`cnet.dll` 与 `salts.dll`；仅系统 PATH 下 `tidessqld --version` 退出码为 0。
runtime stop 在配置的总 shutdown deadline 内以最长 50 ms 切片重试 CNet BUSY；只有 server
quiescent 后才关闭数据库。server E2E 另以活动、尚未完成 TLS 的连接强制执行零超时 stop，
验证 BUSY 保留 owner、停止后拒绝 poll/新准入，并可在后续正常 deadline 内重试至完整释放；
该用例单独连续运行 20 次通过（每次 22 条断言）。

`orm_mysql_tidessqld_e2e` 另以动态加载的 MySQL ORM plugin 连接该独立 daemon，覆盖
TLS/full-auth、raw prepared INSERT、结构化 SELECT/UPDATE、SERIALIZABLE 事务与回滚
可见性。MySQL plugin 的 capability 元数据现同步声明 `ORM_DRIVER_CAP_SERIALIZABLE`，
与 backend 映射及上述真实链路一致；进程 fixture 由两条测试共享，仍只属于测试代码。

可选的 `tidessqld_connector_j_e2e` 使用官方 MySQL Connector/J jar 启动真实 Java
进程，经 `VERIFY_IDENTITY` TLS、`caching_sha2_password` full-auth 连接 daemon，再完成
关闭 autocommit、建表、参数化 INSERT、COMMIT 和参数化 SELECT。配置时显式提供
`TURBODB_MYSQL_CONNECTOR_J_JAR=<absolute-jar>` 才注册该测试，不把 Java 或 Connector/J
变成生产依赖。Connector/J 8.3.0 与 9.1.0 已分别通过。
现有 native SDK workflow 在 Windows 与 Linux host job 固定获取 9.1.0 并启用同一测试；
Android 交叉编译 job 不下载 Java 或 Connector/J。

当前远程客户端验收矩阵如下；“通过”只表示列出的链路和版本，不外推到未测试版本：

| 客户端 | 实际版本/边界 | 已验证链路 |
|---|---|---|
| `TurboDB::MySQL` | 与当前工作树同版本 | TCP、TLS 1.3、full-auth、prepared DDL/DML/query、事务、保存点、autocommit、断连回滚与 COMMIT_UNKNOWN |
| MySQL ORM plugin | Driver Plugin ABI 4，与当前工作树同版本 | 独立 `tidessqld`、raw prepared DML、结构化查询/更新及 SERIALIZABLE rollback |
| MySQL Connector/J | 8.3.0、9.1.0；Java 17 | 独立 `tidessqld`、`VERIFY_IDENTITY`、full-auth、关闭 autocommit、DDL、参数化 INSERT/SELECT 与 COMMIT |

Connector/J 9.1.0 在当前服务版本下实际执行并由集成测试固定验证的初始化投影为：

```sql
SELECT @@session.auto_increment_increment AS auto_increment_increment,
       @@character_set_client AS character_set_client,
       @@character_set_connection AS character_set_connection,
       @@character_set_results AS character_set_results,
       @@character_set_server AS character_set_server,
       @@collation_server AS collation_server,
       @@collation_connection AS collation_connection,
       @@init_connect AS init_connect,
       @@interactive_timeout AS interactive_timeout,
       @@license AS license,
       @@lower_case_table_names AS lower_case_table_names,
       @@max_allowed_packet AS max_allowed_packet,
       @@net_write_timeout AS net_write_timeout,
       @@performance_schema AS performance_schema,
       @@query_cache_size AS query_cache_size,
       @@query_cache_type AS query_cache_type,
       @@sql_mode AS sql_mode,
       @@system_time_zone AS system_time_zone,
       @@time_zone AS time_zone,
       @@tx_isolation AS transaction_isolation,
       @@wait_timeout AS wait_timeout;
```

该语句的列顺序、类型和值由
[`connection_test.c`](tests/integration/connection_test.c) 的正式 21 列用例约束；驱动后续
发送的事务与 prepared SQL 则由真实进程 E2E 约束。

HIGH｜当前 GmSSL 3.2.0#3 会拒绝 JDK 默认 ClientHello 中空的 OCSP
`status_request` 扩展；不设置开关时真实测试稳定在 TLS 握手阶段失败。测试进程仅设置
`-Djdk.tls.client.enableStatusRequestExtension=false`，其余 TLS version、cipher、签名算法、
命名组和 session ticket 使用 JDK/Connector/J 默认值，证书链与 `localhost` 身份验证仍启用。
这是一项明确的 GmSSL/JDK 部署兼容限制，不代表默认 Java TLS 已完全兼容；正确修复边界
在 GmSSL/CNet provider，不能通过关闭 `sslMode=VERIFY_IDENTITY` 或明文回退规避。

```powershell
cmake --preset win-release-user `
  -DTURBODB_MYSQL_CONNECTOR_J_JAR=C:/path/to/mysql-connector-j-9.1.0.jar
cmake --build --preset win-release-user --target tidessqld_connector_j_e2e_test
ctest --preset win-release-user -R '^tidessqld_connector_j_e2e$' --output-on-failure
```

事实｜2026-10-06 `install-win-release-user` 已重新构建并安装当前 Release 树；已安装
`tidessqld --version` 输出 2.0.1，安装 manifest 与 package export 不包含 Redis ORM driver，
driver 目录仅有 SQLite、PostgreSQL、MySQL、TidesDB 四个插件。独立 `TurboDB::Redis`
client/header/export 仍保留，未被误删；复用旧安装前缀遗留的单个
`turbodb_driver_redis.dll` 已按精确路径清除。重新安装后以只含系统目录的 PATH 运行
`tidessqld --version` 仍成功；`tidessqld_config/process_e2e/cli/connector_j_e2e`、
`orm_driver_interface`、`orm_driver_interface_cpp`、`orm_mysql_plugin` 与
`orm_mysql_tidessqld_e2e` 八项 Windows Release CTest 顺序执行全部通过（5.12 秒）。
进程测试以测试专用绝对路径覆盖直接启动上述已安装 `tidessqld.exe`，CLI、现有 MySQL
driver 事务/保存点、Connector/J 和 MySQL ORM plugin 四项再次顺序通过（4.22 秒）；
默认构建树路径的同四项也通过（4.24 秒）。覆盖路径不存在或不是绝对文件时不回退 PATH。
Windows/Linux native workflow 在 install 后、stage 前使用各自安装树执行同一四项；本地仅
验证 Windows，Linux 结果须由实际 runner 产生后才能计入验收。入口 workflow 的 push/PR
过滤已覆盖 `tidessql/**`、packaging/cmake/presets、CMake preset 与 vcpkg manifest/config，
这些范围的独立改动不会再绕过 host matrix。
Native SDK 暂存和 NuGet 内容测试现将 Windows/Linux 的 `bin/tidessqld[.exe]` 作为必需
发布文件，并明确拒绝 Android 包误带 host daemon；Redis ORM driver 的负断言保持不变。
NuGet 内容测试已按当前安装规则改为要求 GmSSL config、头文件、许可证及 Release/Debug
静态库，同时按目录/库名前缀拒绝遗留 OpenSSL config/header、BoringSSL copyright、
crypto/ssl 开发库及 pkg-config 文件。Windows 的 PostgreSQL 驱动依赖链经 `dumpbin` 确认为
`turbodb_driver_postgresql.dll → libpq.dll → ssl.dll/crypto.dll`，因此这两个运行时 DLL 保留；
MySQL/TidesSQL 的开发接口和静态链接仍只使用随包 GmSSL。staging 不再复制 vcpkg 整个 `bin`，
而是 fail-fast 复制 `sqlite3.dll`、`libpq.dll`、`ssl.dll`、`crypto.dll` 四项已验证闭包；内容测试
精确约束 Windows `bin` 的八个 DLL，防止 ECPG/pkgconf 等无关文件泄漏。事实｜新 Windows
Release install manifest 生成成功，fresh stage 含四种 ORM driver、`tidessqld.exe`、TidesSQL/
GmSSL SDK 且不含 Redis ORM driver 或旧加密开发文件；正式 staged consumer 配置、编译、链接、
受限 PATH 运行均通过，stage daemon 的 process/CLI/Connector/J/ORM E2E 4/4 通过。Python 测试文件通过
语法检查；本机没有 Linux/Android 安装树或 .NET SDK，因此完整三 RID nupkg 内容测试仍须
由实际 CI runner 验证。
同一内容测试还明确要求三个 RID 都包含 TidesSQL/TidesDB CMake config、公开 TidesSQL 头和
平台静态库；Windows install manifest 已与这些路径逐项核对。

HIGH｜安装后 consumer 审计发现并修复两个发布阻断：Windows 原生反斜杠 SDK 环境路径经
vcpkg `find_package` wrapper 解析时产生非法 CMake 转义；`TurboDBConfig.cmake` 又在
`TurboDB::Types` 定义前加载引用它的 `OrmTargets.cmake`。导出配置现先规范化依赖根路径，
按依赖 → `TurboDBTargets` → `OrmTargets`/`TidesSQLTargets` 顺序加载；组合安装的独立
`TidesSQLConfig.cmake` 也显式解析其导出文件所引用的随包 GmSSL。正式 consumer 使用原生
Windows 路径，依次完成 `find_package(TidesSQL)`、`find_package(TurboDB)`，编译链接
`TurboDB::TidesSQL`、`TurboDB::MySQL`、`Orm::C`，从导出的 `TurboDB_DRIVER_DIR` 加载已安装
MySQL plugin、拒绝同目录出现 Redis ORM plugin 并核对 canonical driver metadata，再以
仅含三个安装 SDK bin 与系统目录的运行时 PATH 执行 ABI/生命周期 smoke，全部通过。
fresh stage 也通过同一 plugin load 验证。
Windows/Linux native workflow 在安装后及 staging 后各运行一次同一 consumer；Linux 结果仍须
实际 runner 验证。

事实｜2026-10-06 使用已发布 Salts 1.8.27 重新配置 `ci-windows-release`，Connector/J
9.1.0 在用户 preset 下连续 3/3 通过。CI preset 首轮 143 项 CTest 中唯一失败是从普通
PowerShell 启动时架构测试找不到 MSVC `dumpbin`；在
`VsDevCmd.bat -arch=x64 -host_arch=x64` 环境完整重跑后 143/143 通过（350.51 秒）。随后
`install-ci-windows-release` 成功；fresh staged SDK manifest 固定 `salts=1.8.27`、`saltsutils=4.1.10` 及
`drivers=sqlite,postgresql,mysql,tidesdb`。安装树与 staged SDK 的独立 consumer 均完成
configure/build/runtime smoke，staged `tidessqld` 的 process、CLI、Connector/J 和 MySQL
ORM 四项 E2E 为 4/4 通过（5.12 秒）。这些结果补齐 Windows 安装与真实客户端验证；Linux、
Android 及 NuGet 三 RID 仍必须由提交后的实际 CI runner 验证，不能由本机结果代替。

事实｜Windows Debug AddressSanitizer 的 engine-only 配置已构建并顺序执行
`tidessql_prepared`、`tidessql_connection`、`tidessql_parameters`、
`orm_tidesdb_sql_runtime`、`tidessql_mysql_dispatch` 和
`tidessql_mysql_server_transaction_e2e`，6/6 通过（119.70 秒）。该配置不混用 Release
依赖。随后从 SaltsUtils 干净 `HEAD ec6682d133e6` 构建并安装 Debug+ASan 生产包，完整
ORM/daemon 配置顺序执行 `tidessqld_config/process_e2e/cli/connector_j_e2e`、
`tidessql_mysql_server_e2e`、`orm_driver_interface`、`orm_driver_interface_cpp`、
`orm_mysql_plugin` 与 `orm_mysql_tidessqld_e2e`，9/9 通过（17.06 秒）。SaltsUtils 自身
tests/examples/benchmarks 未纳入该依赖安装；上述结果验证的是 TurboDB 实际消费链路。

MED｜登录速率/CPU 配额、高容量组合的聚合内存预算、Linux CI 的实际 runner 结果、
benchmark 和上述默认 JDK OCSP ClientHello 兼容仍待继续。
ORM Driver Plugin ABI 已从 3 升至 4，旧插件二进制必须重编译；MySQL wire 与磁盘格式未变。

`tdsql_transaction_release_checked()` 和 `tdsql_result_destroy_checked()`
返回清理结果并消费 handle，失败会隔离连接；无论返回值如何都不得再使用
已消费 handle。connection_close 则在失败时保留连接。所有错误对象先调用
`turbodb_error_init()`；affected 和读取 DTO 在失败时保持不变。COMMIT_UNKNOWN
必须关闭连接，不能重放提交。

SDK 随正常安装 preset 发布公共头、静态库及 CMake package：

```powershell
cmake --build --preset install-win-release-user
```

消费者使用匹配架构、工具链和 Salts SDK，在 preset 中设置包根后调用
`find_package(TidesSQL 1 CONFIG REQUIRED)` 并链接 `TurboDB::TidesSQL`；静态
依赖由 target 传递，不需要 ORM runtime、driver 插件或 SaltsUtils。
`find_package(TurboDB CONFIG REQUIRED)` 也继续提供同一 target。独立构建时
开启 `TURBODB_BUILD_TIDESSQL` / `TURBODB_BUILD_SQLPARSER`，关闭 ORM/dbtools
即可省去 SaltsUtils 与 ORM 的 Crypto 依赖。正常包中只安装此 SDK 的公共头，
不会安装 budget、scan、schema 等私有实现头。

完整 C 示例是 [examples/query.c](examples/query.c)，启用 BUILD_EXAMPLES 后
构建 `tidessql_example`。`tidessql_example DATABASE --initialize` 明确初始化
example CF，查询并打印 `42`；之后使用 `tidessql_example DATABASE` 打开。
重复初始化已有 CF 会返回错误，不会覆写数据。

`drivers/tidesdb` 负责插件注册、结构化 ORM plan 的 MySQL 渲染和
CFlow/CSerde 结果编码；`relational_backend.c` 通过这些入口适配，不再保存
连接或事务状态。直接入口的完整使用与生命周期验证见
[connection_test.c](tests/integration/connection_test.c)，该测试只链接引擎和 TinyTest，
不依赖 ORM runtime 或插件。拆分决策和迁移顺序见
[模块边界](design.md#tidessql-模块边界与迁移)。

在匹配 preset 的 VS 开发环境中，可复验核心与 ORM 适配回归：

```powershell
cmake --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user -R "^(orm_|tidessql_)" --output-on-failure -j 2
```

验证范围包括直接连接入口、核心单元/原生集成、ORM 插件、事务 owner、
关系结果所有权与原生 WAL 故障。原生故障测试只链接
故障版 native core，避免生产 TidesDB 依赖混入后使故障注入失效。
公共 C/C++ 调用、ABI 拒绝、checked cleanup 和 ORM DTO 兼容性也纳入正式测试。
2026-10-05 的 `win-release-user` 完整构建与上述 94 项 CTest 全部通过；
`install-win-release-user` 已成功安装 SDK 公共头、静态库和两个 package 导出。
Linux/sanitizer 尚未执行：本机没有 WSL 发行版和匹配 Debug Salts SDK；
不能用 Release 依赖代替对应 profile 的验证。SDK 为静态库，本次不提供
跨编译器/架构二进制兼容或动态插件 ABI。

后续关系执行层的架构、存储与事务边界、分阶段计划见
[TidesDB SQL 执行层设计](design.md)。该文档记录架构决策与分阶段验证；下文描述当前已实现的能力。
M0 存储契约、WAL 故障与并发测试已落地；ORM 只使用关系执行器，范围见下文。

M5 已开放 `CREATE INDEX` / `CREATE UNIQUE INDEX`：支持 I64/U64/DOUBLE 普通/唯一复合列键、
ASC/DESC 和可空列，在同一事务回填已有数据。索引名在表内唯一；非 NULL 唯一键重复
返回 CONSTRAINT，含 NULL 的复合键允许重复。表达式、前缀、部分索引及其他键类型
仍明确拒绝。定义、键和目录协议见 [index.h](src/index.h)、[index_record.h](src/index_record.h)。

CREATE TABLE 也支持表级具名或匿名 `KEY [name](...)`、`INDEX [name](...)`、
`UNIQUE [KEY|INDEX] [name](...)`、列级 UNIQUE，以及 MySQL
`[CONSTRAINT [symbol]] UNIQUE` 写法，包含数值复合键、NULL 和 ASC/DESC。所有定义先完成验证，
表、版本、全部索引目录及对象 ID 在同一个批次发布，失败不会留下半张表或部分索引。
不隐式提交，支持用户事务/savepoint 回滚；IF NOT EXISTS 命中既有表不会追加索引。
当 UNIQUE 同时给出 symbol 和 index name 时使用 index name；缺少 index name 时使用 symbol，
两者都省略时按首列名生成。`CONSTRAINT [symbol] PRIMARY KEY` 规范化为固定名称 PRIMARY；
列级裸 `KEY` 按 MySQL 规则等价于 PRIMARY KEY；列同时声明 PRIMARY KEY 与 UNIQUE 时
保留两项索引。上述范围之外的键类型仍明确拒绝。

[索引构建](src/index_store.h) 与 [索引维护](src/index_change.h) 共享原生 SERIALIZABLE owner。
INSERT、UPDATE、DELETE、主键移动会把全部 Index/Unique 变化与 Data、TableVersion
放入同一个原子批次；raw SQL 和结构化 ORM 共用这条路径。没有 DDL 隐式提交：
显式事务与用户 savepoint 可以回滚索引创建和删除，失败不留下半个索引或半行更新。

支持 `DROP INDEX name ON table`，成功 affected=0。在同一事务快照中由 Data 推导键、
核对实际索引项，然后原子删除该索引的 Index/Unique/目录并更新 TableVersion。
表数据和其他索引保留；删除唯一索引后解除其约束，可使用同名创建新索引。
索引不存在返回 SQL_ERROR；PRIMARY、限定名称、SQLite 的 IF EXISTS 形式及
ALGORITHM/LOCK 不在执行范围。语法依据 [MySQL DROP INDEX](https://dev.mysql.com/doc/refman/8.4/en/drop-index.html)。
索引缺项、孤儿键或损坏数据返回 DATASTORE_ERROR，事务必须回滚。

DOUBLE 键使用有限 binary64 数值顺序，正负零映射为同一唯一键；NULL 与数值零分离。
NaN/Inf 输入和损坏的非规范键均拒绝。整数键既有字节保持不变，原子回填、DML、
REPLACE、IGNORE、用户事务/保存点及重开均复用同一目录与索引维护路径。

**MED｜兼容边界：** 首次成功提交整数索引会将该 CF 的 Manifest 从 v1 升到 v2；
首次成功提交 DOUBLE 索引则从 v1/v2 升到 v3。含 DOUBLE 的索引记录为 wire v2，
纯整数记录保留 wire v1；schema/Data 编码不变，新驱动继续读取既有 v1/v2 CF。
旧驱动会拒绝超出其支持版本的 CF。删除最后一个索引或后续创建整数索引/表均不会
降低 Manifest，计数器不变、索引 ID 不回收，尚无格式降级入口。升级只随显式建索引
事务提交发生，普通打开和已有数据读写不自动迁移；用户已确认 v3 兼容性边界。
实现与验证见 [DOUBLE 索引协议](design.md#double-二级索引键与格式接入协议)。

支持 `DROP TABLE [IF EXISTS] name [, name] ... [RESTRICT|CASCADE]` 和单表
`TRUNCATE [TABLE] name`，成功 affected=0。按 MySQL 规则，RESTRICT/CASCADE 是无操作兼容词。
DROP 原子删除各表数据、所有索引项/定义及表目录；TRUNCATE 清空数据和索引项，保留
表定义、索引定义与 ID。两者先验证全部目标的数据/索引一致性，再以一个受预算限制的
批次写入，事务和用户 savepoint 均可回滚。多表 DROP 未写 IF EXISTS 时，只要一个目标
不存在，整条语句在写入前返回 SQL_ERROR；写有 IF EXISTS 时跳过缺失目标。空表 TRUNCATE
也更新 TableVersion；DROP 不复用 ID、不降低 Manifest 格式版本。TEMPORARY 和限定名称
仍返回 UNSUPPORTED。
沿用本项目的可回滚 DDL 契约，不执行 MySQL 服务端的隐式提交；大表超过现有
读写/工作内存限额会明确失败，不拆批提交或后台清理。实现见 [table_clear.h](src/table_clear.h)。

SELECT 已支持等值、左前缀和多区间索引读取：单表 WHERE 由数值列与常量/参数的
`=`、`<=>`、`IS NULL`、`<`/`<=`/`>`/`>=` 或正向 `BETWEEN` 通过 AND 连接，
每个 AND 分支还允许一个正向标量 `IN` 列表；最外层可用 OR 连接这些分支。
选择目录中首个能用于所有分支的索引，每个分支使用连续等值前缀，并可在下一列
增加范围。区间排序后合并重叠、重复和相邻部分，每行最多读取一次；范围列后的
条件及原始 WHERE 仍会复核。普通/唯一索引均可读取，排序/分页/聚合继续
使用现有执行器。参数每次运行重新编码；LIMIT 0、取消和 EXPLAIN 不读取 Data。
EXPLAIN 的 `key`/`possible_keys` 为选中候选，`type` 为 ref、range 或完整非 NULL
唯一键的 const；多区间计划为 range，`key_len` 为各分支使用的本地编码前缀
字节数的最大值，range 的 ref 为 NULL。
rows/filtered 仍未知，非 MySQL 引擎成本估计。ASC/DESC、开闭边界、NULL 和整数
类型边界均按现有编码处理，IN 中的 NULL 不匹配 NULL。无共同前导索引、AND 内
嵌套 OR、同一 AND 中多个 IN、NOT IN、表达式条件和 JOIN 继续扫描；跨索引合并、
跳跃扫描和成本选择尚未实现。
DOUBLE 范围限定为有限数，开闭端点正确处理次正规数和零附近的编码空隙。
DOUBLE 列可用整数/DOUBLE probe；整数列与 DOUBLE probe 比较仍使用普通扫描，
避免舍入造成多个整数相等而索引定位漏行。两种零的 IN/OR 区间会合并，避免重复行。
索引定位后回表核对实际元组及唯一占用；仅检查触及的条目，不等价于全库一致性审计。
接口和生命周期见 [index_lookup.h](src/index_lookup.h)。不宣称完整 MySQL 兼容。

支持 `ALTER TABLE t RENAME [TO|AS] u` 和 `ALTER TABLE t RENAME COLUMN a TO b`。
在同一原子批次修改目录和 TableVersion，表改名同步更新索引记录中的表名；列改名
保留索引引用的列序号。主键、列类型、对象 ID、数据与索引键值不变，affected=0，
可由事务/savepoint 回滚。缺失表或列、重复列名明确报错，目标表已存在返回
CONSTRAINT。名称沿用大小写敏感 ASCII 约定，禁止限定名称。只扫描元数据，
受已有预算限制，不做格式迁移。协议及可复验用例见 [table_alter.h](src/table_alter.h)。

支持 `ALTER TABLE t ADD [COLUMN] c BIGINT|DOUBLE`（BIGINT 可 UNSIGNED，
可声明 NULL/NOT NULL 和有限数值/NULL 常量 DEFAULT）及 `ALTER TABLE t DROP [COLUMN] c`。ADD 默认追加尾列，
支持 `FIRST` 或 `AFTER existing_column` 指定位置；目标列不存在报 SQL_ERROR。DROP
仅删除非主键、非索引列，自动调整剩余主键/索引的列序号。ADD 为旧行填入显式默认值，
没有显式默认值的可空列补 NULL；
DROP 保留其余列值；旧行和完整 Data/Index/Unique 命名空间先通过校验，再在同一
原子批次重写数据和目录，保留对象 ID、索引物理键及 Data/索引 wire 版本。可事务/savepoint
回滚，成功 affected=0，无隐式提交。整表快照及新行编码受语句内存、物化行数和
读写预算限制，超额返回 LIMIT_EXCEEDED，不分批提交。非空表 ADD NOT NULL 没有默认值时返回
UNSUPPORTED；索引依赖须先 DROP INDEX；损坏、缺失或孤儿索引项要求事务回滚。
支持 `ALTER TABLE t ALTER [COLUMN] c SET DEFAULT literal/(expr)` 和 `DROP DEFAULT`，
使用 CREATE 的数值/NULL 常量规则。SET/DROP 只更新目录、版本和 Manifest 屏障，保留
旧行、类型、主键及索引键；后续省略写入、裸 DEFAULT 和 UPDATE DEFAULT 使用新定义。
DROP 后可空列省略写入得到 NULL，NOT NULL 列在严格路径中缺值仍报错。支持事务和
savepoint 回滚，无隐式提交，沿用已有 schema wire v1/v2，无新版本或数据迁移。
类型变更和多动作 ALTER 尚未开放。解析器接受单个 `ALTER TABLE ... ADD ... FOREIGN KEY`，
但关系执行器尚未实现外键约束，因此明确返回 UNSUPPORTED，不修改目录或数据。

## 连接配置

TidesDB ORM 插件仅使用 TidesSQL 关系执行器。`sql_profile` 已删除，传入该选项即报 INVALID_ARGUMENT；旧 ORMTDB 格式不再支持，不自动迁移或降级。
执行器使用关系 Catalog 与数值表，支持 raw SQL 的 CREATE/DROP/TRUNCATE TABLE、ALTER TABLE rename/默认值/有界增删列、CREATE [UNIQUE] INDEX、DROP INDEX、
SELECT、INSERT、UPDATE、DELETE、SHOW TABLES/COLUMNS/INDEX/CREATE TABLE、EXPLAIN SELECT，以及 ORM 显式 SERIALIZABLE 事务和连接级 SQL 事务。
raw SELECT 已支持数值/BOOL/NULL 输出的 DISTINCT 去重，可与 ORDER BY、LIMIT/OFFSET 组合。
也支持 GROUP BY 列/表达式/别名/序号、COUNT/MIN/MAX、DOUBLE SUM/AVG、方差/标准差、数值位聚合和 HAVING，含全局聚合及排序分页；普通 COUNT/SUM/AVG/MIN/MAX 的 DISTINCT 范围见下文。
SELECT/最终 ORDER BY 支持排名/分布函数、LAG/LEAD 和 FIRST/LAST/NTH_VALUE、内联及命名窗口及帧，范围见下文窗口阶段。
raw SELECT 已接入 INNER/LEFT/RIGHT/CROSS JOIN、自连接和多表链，复用同一 Catalog 事务快照。
也支持显式 LATERAL 派生表：按当前前缀行重开查询，可组合普通派生表、CTE、相关子查询、
既有聚合和排序分页；配置递归上限后可用于递归 CTE 内外。方言和边界见下文。
也支持 UNION、INTERSECT、EXCEPT 的 ALL/DISTINCT、混合链、括号分支及独立的外层排序分页。
支持无 FROM 的 SELECT：常量、参数及已有表达式以一行输入执行，也可组成 UNION、子查询和 CTE。
标量及 IN/NOT IN 子查询已接入 raw SELECT；非相关查询支持嵌套、JOIN/分组/UNION 与只读 EXPLAIN，
普通单表及仅含普通表 JOIN 的外层 SELECT 还支持直接、显式限定的相关 scalar/IN/EXISTS 查询。
显式设置正整数 `sql_max_recursive_iterations` 后，支持有界 `WITH RECURSIVE` 查询及其
default/TRADITIONAL EXPLAIN，包括全局 LIMIT/OFFSET、多个成员、重复引用和已有 JOIN/依赖。
省略该选项仍拒绝递归自引用；`WITH RECURSIVE` 中没有自引用的定义不需要该选项。

连接必须指定 `path` 和 `column_family`。首次在独占、静止数据库中设置
`sql_initialize=true` 创建新 SYNC_FULL CF 和 Manifest；CF 已存在则拒绝初始化。
之后省略该选项或设为 false，只打开已初始化的关系 CF。初始化失败保留 CF，不删除数据。
此入口不迁移旧库，不隐式提交 DDL。

使用已有 `orm_raw()`、`orm_query_bind()`、`orm_query_execute()` 或 Publisher API。
SELECT/SHOW/EXPLAIN 用行 Publisher，其余受支持语句用命令 Publisher；命令在 resume 时执行。
同一连接至多一个活动事务/查询，显式事务的查询关闭前下一语句或结束事务返回 BUSY。
结构化 ORM 的 SELECT/INSERT/UPDATE/DELETE 已接入同一关系执行器，支持字段选择、
全部字段、AND 条件、六种普通比较、NULL 相等/不等条件及 SELECT ORDER BY、LIMIT/OFFSET。
表须先通过 CREATE TABLE 声明；不自动建表或推断类型。关系数值列上的 LIKE、
非数值表类型和其他显式隔离级别仍返回错误，不静默忽略。M2 数值表最小闭环已接通。
可编译的完整调用见 [真实插件测试](../orm/tests/integration/tidesdb/sql/relational_test.c)。

结构化计划复用 [共享 MySQL 渲染器](../orm/src/sql/orm_mysql_render.h)，
参数经占位符传入，再走同一 MySQL parser 和 runtime。名称按部分加反引号，
避免关键字列名变成 SQL 常量；限定列名可用 `items.id`。渲染文本受 max_query_bytes
限制（包含引用字符），临时 SQL/参数指针与 parser 属于前端内存，不计入执行 WORK；
转换后的执行值数组计入 WORK。结构化查询和 raw SQL 共享目录、行、事务与保存点。

用户保存点通过 `orm_transaction_savepoint()`、`orm_transaction_rollback_to_savepoint()`、
`orm_transaction_release_savepoint()` 或 SQL 文本 `SAVEPOINT name`、
`ROLLBACK [WORK] TO [SAVEPOINT] name`、`RELEASE SAVEPOINT name` 使用。
显式 ORM 事务内两种入口共享同一注册表，可交替调用；SQL 名称支持反引号，
不接受绑定参数，成功 affected=0，命令 Publisher 在 resume 时执行。
自动提交下 SAVEPOINT 成功但不保留点，随后 ROLLBACK TO/RELEASE 返回 SQL_ERROR。
SQL 生命周期支持 `BEGIN [WORK]`、`START TRANSACTION [READ WRITE|READ ONLY]`、
`COMMIT/ROLLBACK [WORK] [AND [NO] CHAIN] [NO RELEASE]`，成功 affected=0。
开始后普通连接查询、raw SQL 和结构化 ORM CRUD 共用同一 SERIALIZABLE owner，
不必传入 ORM transaction handle；结束后沿用会话的 autocommit 模式（默认开启）。SQL 再次 BEGIN 会先提交旧事务，
CHAIN 在结束后开始新事务并保留只读模式；保存点不跨事务保留。
再次执行没有访问模式的 BEGIN 也保留当前只读模式；显式 START TRANSACTION READ WRITE 可覆盖它。
READ ONLY 拒绝执行器 的表/索引 DDL 和 DML，不能与 ORM 显式事务并存。
ORM transaction handle 内的 SQL 生命周期命令继续拒绝，必须通过该 handle 的 API 结束。
`RELEASE`、`WITH CONSISTENT SNAPSHOT` 未开放，
在改变事务状态前明确拒绝。关闭活动 cursor 前，后续命令及事务控制返回 BUSY。
取消未执行的命令不改变事务；最终连接释放会回滚未结束事务。
提交冲突需要完整 ROLLBACK 确认；COMMIT_UNKNOWN 隔离连接，不自动重试。
若旧事务成功结束后，新事务或 CHAIN 创建失败，返回错误并说明旧事务已结束，
连接没有活动 SQL 事务。详见 [连接级生命周期协议](design.md#连接级-sql-事务生命周期协议)。
支持 `SET [SESSION|LOCAL] TRANSACTION READ ONLY|READ WRITE`、
`ISOLATION LEVEL SERIALIZABLE` 及两者组合，成功 affected=0。省略作用域时只影响下一事务，
活动 SQL/ORM 显式事务内拒绝；SESSION/LOCAL 改变以后事务的默认模式，不改变当前事务，
且设置不随 ROLLBACK 或 ROLLBACK TO 撤销。开始时优先用显式 START 模式、下一事务设置，
再用会话默认值；SQL BEGIN、ORM SERIALIZABLE API 和自动事务共享规则。
只改指定特征；在事务间设置 SESSION 访问模式会覆盖未使用的同名下一事务设置。
CHAIN 与无模式的再次 BEGIN 保留当前模式；非 CHAIN 完成后恢复会话默认值。
GLOBAL 和其他隔离级别仍拒绝，不静默改为 SERIALIZABLE。
MED｜执行器 所有自动查询都有 Catalog 事务，SHOW、EXPLAIN 和常量 SELECT 也消费
下一事务设置；尚未模拟 MySQL 未访问事务表时的特征保留规则。
详见 [事务特征协议](design.md#sql-事务特征的-session-与下一事务协议)。
支持单项 `SET [SESSION|LOCAL] autocommit=value` 及 `@@autocommit`、
`@@SESSION.autocommit`、`@@LOCAL.autocommit` 写法，成功 affected=0。
值可用整数 0/1、TRUE/FALSE、ON/OFF、文本 ON/OFF、DEFAULT、类型化参数及已支持的纯
表达式；不把 DOUBLE、NULL、任意文本或越界整数自动转成布尔值。先完成参数/表达式/预算
校验和 cleanup，再改变模式或提交。GLOBAL、用户变量、多项 SET 及未列出的变量赋值继续拒绝。
关闭自动提交后，首条普通命令/查询创建连接 owner，后续 raw/结构化 CRUD、保存点和
查询共用事务；COMMIT/ROLLBACK 后模式仍关闭，下条语句开始新 owner。
从 0 切换为 1 才隐式提交，提交成功后开启自动提交；同值设置不提交，包含 BEGIN 内
重复设置 1。模式不随回滚撤销，新连接恢复开启；最终释放回滚尚未结束的事务。
ORM handle 内的 autocommit 命令明确拒绝，以保留该 handle 的提交/结束状态。
详见 [autocommit 协议](design.md#sql-autocommit-的连接状态与提交协议)。
单项 SET 也支持 `transaction_read_only` 与 `transaction_isolation`，RHS 可使用
类型化参数、已支持的纯表达式和会话变量读取。只读值使用上述布尔转换，隔离接受
ASCII 大小写不敏感的 `SERIALIZABLE` 或枚举序号 3；其他已知隔离级别返回 UNSUPPORTED，
非法类型或值返回 SQL_ERROR。DEFAULT 恢复执行器 的固定默认（READ WRITE/SERIALIZABLE）。
按 [官方事务变量作用域](https://dev.mysql.com/doc/refman/8.4/en/set-transaction.html)，
`SET transaction_read_only=…`、SESSION/LOCAL 和 `@@SESSION`/`@@LOCAL` 修改会话默认；
`SET @@transaction_read_only=…` 只影响下一事务，在活动事务内拒绝。隔离变量同理。
设置不打开 Catalog 事务、不消耗下一访问模式、也不改变活动事务的冻结模式；
会话设置不随回滚撤销。ORM transaction handle 允许 session 设置，仍拒绝 autocommit
与下一事务设置。只有求值、预算与 statement 清理全部成功才发布状态。
MED｜未实现全局可变默认、多项 SET 和其他隔离级别；完整协议与复验步骤见
[SET 事务变量](design.md#set-事务变量的单项写入协议)。
已接入三个可写会话变量的表达式读取：`@@autocommit`、`@@transaction_read_only`、
`@@transaction_isolation`，支持 SESSION/LOCAL 作用域、ASCII 大小写和既有 backtick
标识符规则。前两项返回 I64 0/1，隔离返回 TEXT `SERIALIZABLE`；访问模式是会话
默认值，与当前事务及下一事务覆盖项分开。语句快照传递给 SELECT、依赖查询、
INSERT/UPDATE/DELETE 及受支持 SET 的 RHS。

Connector/J 初始化另可读取只读元数据：`auto_increment_increment`、四个
`character_set_*`、两个 `collation_*`、`init_connect`、三个 timeout、`license`、
`lower_case_table_names`、`max_allowed_packet`、`performance_schema`、两个
`query_cache_*`、`sql_mode`、`system_time_zone` 和 `time_zone`；`tx_isolation` 是隔离变量
的兼容别名。`max_allowed_packet` 来自本次 request 的 `max_query_bytes` 硬上限，字符集为
`utf8mb4`/`utf8mb4_bin`，query cache 与 performance schema 为关闭，SQL mode 为
`NO_BACKSLASH_ESCAPES,STRICT_TRANS_TABLES`。无法从同步 SDK 快照如实得出的 timeout 返回
SQL NULL，不伪造动态服务配置；`SHOW VARIABLES` 的固定 TEXT `Value` 列将这些 NULL
呈现为文本 `NULL`。新增元数据不允许 SET。

`SHOW [SESSION|LOCAL] VARIABLES` 枚举同一有界变量表，Variable_name/Value 为 TEXT，
布尔显示 ON/OFF；LIKE 使用 ASCII 不区分大小写匹配，WHERE 可使用实际结果列和类型化
参数；GLOBAL/用户变量/未知变量明确拒绝。
普通表达式 LIKE 的既有大小写敏感规则保持不变。缺少连接快照的私有执行入口
不允许变量实际求值。
Salts Plugin 已切换为 ABI 4，旧 epoch 在加载时拒绝；升级 SDK 时须清理旧编译
产物，协调重建 core 与全部驱动。ABI 接入时清理重建后 30/30 个相关 CTest 目标通过，
包括真实插件 282 个用例、私有执行器 223 个用例、旧 epoch 拒绝、跨驱动和 C/C++ flow。
后续 SHOW 过滤增量的插件 287 个用例、私有执行器 229 个用例通过。
协议与范围见 [会话快照](design.md#会话变量只读快照接入协议)。
采用 [MySQL 保存点规则](https://dev.mysql.com/doc/refman/8.4/en/savepoint.html)：
同名替换为最新点，回滚保留目标并删除后续点，release 不提交事务。
名称限 `[A-Za-z_][A-Za-z_0-9]*`、最多 63 字节，ASCII 大小写不敏感。
注册表首次使用时按配置上限固定分配，占用 retained WORK，事务结束释放；
释放或回滚可复用槽位，容量满时仍允许同名替换。回滚不退还累计读写额度。
原生保存点操作失败后只能整体回滚；名称、预算或分配错误不会使事务失效。
协议与兼容性边界见 [SQL 文本保存点协议](design.md#sql-文本保存点接入协议)。

关系执行专属限额（全部为正十进制整数，溢出、重复、未知选项立即拒绝）：

| 连接选项 | 默认值 | 用途 |
| --- | --- | --- |
| `sql_max_work_bytes` | 64 MiB | 共享执行工作内存，包括事务常驻元数据 |
| `sql_max_materialized_rows` / `sql_max_groups` | 各 100000 | 物化/分组容量 |
| `sql_max_ast_nodes` / `sql_max_plan_nodes` | 各 65536 | AST/计划额度；前者也限制 parser |
| `sql_max_join_pairs` | 1000000 | 全部连接算子共享的候选配对上限 |
| `sql_max_execution_steps` | 10000000 | 求值、绑定和扫描步数 |
| `sql_max_write_rows` / `sql_max_write_bytes` | 100000 / 64 MiB | 单语句写入，包含目录与版本 |
| `max_scan_rows` / `max_scan_bytes` | 100000 / 64 MiB | 单语句物理读取，包含目录 |
| `sql_max_transaction_read_rows` | 1000000 | 事务累计读取条数 |
| `sql_max_transaction_read_bytes` / `sql_max_transaction_write_bytes` | 各 256 MiB | 事务累计读/写字节 |
| `sql_max_depth` / `sql_max_stack_entries` | 64 / 4096 | 表达式深度/parser 栈项 |
| `sql_max_recursive_iterations` | 未启用，须显式设置 | 每个递归 CTE 的轮次上限；正 uint64 十进制整数 |
| `sql_max_record_bytes` | ORM `max_parameter_bytes` | 单条持久化记录上限 |
| `sql_max_savepoints` | 64 | 用户保存点上限；不得超过 INT_MAX/2-1 |
| `sql_max_warnings` | 64 | 每连接保留的最近语句告警数；范围 1..65535，完整告警总数仍单独计数 |

事务额度不得小于对应语句额度；工作额度不足会拒绝连接或语句，不自动缩减查询。
parser 使用独立 input/node/stack 硬上限；原生 TidesDB、外层 ORM/Publisher、parser 内存
不计入执行 WORK。ORM 输入参数和结果限额继续生效。结果字节为字段名加值长度
（NULL=0、BOOL=1、数值=8、TEXT/BLOB=实际长度），单行受 max_parameter_bytes 限制；
同名输出列需显式别名，否则不能无歧义映射到 ORM 行对象。

递归限额在连接时解析并复制，重连后由新连接配置决定，不持久化、不用 `sql_max_depth` 代替。
一次轮次计成员工厂打开一次，包括确认收敛的最后空轮；达到全局分页上限则不再开启探测轮。
例如 `sql_max_recursive_iterations=3` 可运行：

```sql
WITH RECURSIVE c(n) AS (
  SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3
) SELECT n FROM c;
```

结果为 1、2、3。超限返回 `ORM_STATUS_LIMIT_EXCEEDED`，不发布部分缓存；表达式类型、语法边界、
共享 WORK/STEP/物化额度及 ORM 结果上限仍有效。该选项不是 MySQL `cte_max_recursion_depth`
会话变量，不提供 SET 或热更新；不支持的递归形状仍明确报错。

事实｜递归驱动准入新增 12 项真实插件测试，关系插件累计 80 项/3472 条断言通过。
[存储契约集成测试](../orm/tests/integration/tidesdb/sql/storage_test.c) 直接通过内部
bridge 验证真实 TidesDB 的事务、WAL 配置和进程退出恢复。它复现原生 iterator 的谓词
写偏差，并验证表级版本键保护方案；该方案尚未接入关系执行器，不能据此声称已支持
批量 SQL 或完整 SERIALIZABLE。

`orm_tidesdb_sql_wal_fault` 在独立、不可安装的原生测试库中注入 WAL 写入前失败、
短写、完整写入后报错，以及部分/完整帧写入后的进程退出；重新打开时检查整个 KV
批次。实测完整帧写入后，即使 commit 返回 I/O 错误且 rollback 成功，恢复仍会重放。
另外覆盖显式 `fdatasync` 调用前/后报错及进程退出：同步后的探针先执行真实同步，
再模拟返回错误。Windows 走 `FlushFileBuffers`，使用 `O_DSYNC` 的平台明确跳过
这组独立同步调用用例，不更改原生同步策略。
[多线程存储测试](../orm/tests/integration/tidesdb/sql/storage_race_test.c)
用 4 个真实线程、每场景 16 轮验证唯一键竞争、表版本键保护和不相干写入；同时核验
旧快照、新读结果与重开后的整批状态。它使用原生库，Salts 只管理测试线程及同步门，
没有替换 TidesDB 文件 I/O。固定规模并发测试不代表长时间压力或性能验收。

新关系执行器的 [私有预算组件](src/budget.h) 已有
[20 项契约测试](tests/unit/budget_test.c)，覆盖联合扣费、
容量峰值、溢出、清理和事务累计读写。它已接入 关系执行器 的绑定、查询、
物化及写入路径，由连接的 sql_max_* 和 max_scan_* 配置约束。原生 TidesDB 内部堆分配与缓存
不由该预算逐次计量；不能将语句预算视为整个进程的内存上限。

[私有标量谓词](src/value.h) 已实现类型绑定、普通比较、NULL-safe equality、IS NULL、
NOT/AND/OR 三值逻辑，并调用联合预算限制执行步骤和字符串扫描。其
[65 项测试](tests/unit/value_test.c) 覆盖类型矩阵、完整真值表、
I64/U64 精度边界、UTF-8/内嵌 NUL、非法值和预算失败，以及数值算术的类型与溢出边界。
数值支持同 kind I64/U64/有限 F64 的 `+ - *` 及一元正号，I64/F64 的一元负号；
NULL 传播，整数溢出或非有限结果立即报错。动态 U64 负号仍拒绝；DOUBLE 混合算术、
整数 DIV 和除法/取模的类型及告警规则见下文。
带符号整数字面量继续精确转换，包含 INT64_MIN；规则见[算术协议](design.md#53-有界数值算术)。
**MED｜兼容性边界：** 这是严格类型、TEXT 字节比较的内部子集，不支持 MySQL 的
完整隐式转换或默认 collation；已通过表达式程序接入 关系执行器 的 SQL 执行。
规则依据见设计文档[值语义](design.md#5-sql-值与表达式语义)。

[私有表达式程序](src/expr.h) 已串联 MySQL AST、标量类型和预算，支持 BOOL/NULL/整数/TEXT
常量、已解析的输入槽及上述谓词，还支持标量列表 IN/NOT IN、BETWEEN/NOT BETWEEN、
searched/simple CASE。CASE 选择首个 TRUE 条件或等值比较命中的结果，无 ELSE 时返回
NULL；searched 条件须 BOOL/NULL。CASE/COALESCE/IFNULL 汇总所有数值结果的类型：
只要有 DOUBLE 分支，选中的 I64/U64 结果也转为 DOUBLE，可能舍入大整数；
静态 NULL 不参与 kind 汇总。没有 DOUBLE 的 I64/U64 混合仍需 DECIMAL，其他
非 NULL kind 仍要求相同。CASE nullable 取各结果并集，省略 ELSE 时可空；
COALESCE/IFNULL 只有全部参数可空时才可空。类型由所有分支决定，运行只求值命中路径。
CREATE/ALTER/ADD 的常量默认值在折叠后按声明列类型执行严格赋值转换，
Catalog 仅保存转换后的有限数值/NULL；参见[默认值转换协议](design.md#默认值列赋值转换协议)。
协议与官方依据见[结果类型合并](design.md#casecoalesce-数值结果合并协议)。
COALESCE/IFNULL 支持首个非 NULL 值选择；NULLIF 在普通比较相等时返回 NULL，否则
保留首个值。仅接受裸函数名（大小写不敏感）和正确参数个数；所有参数先绑定，
运行时 COALESCE/IFNULL 短路，NULLIF 两参数各求值一次。没有开放通用函数执行。
支持单参数数值函数 `ABS`、`SIGN`、`FLOOR`、`CEIL/CEILING`，沿用 I64/U64/有限 DOUBLE
的严格类型。ABS/FLOOR/CEIL 保留输入 kind，SIGN 数值结果为 I64 -1/0/1；NULL 传播，
`ABS(INT64_MIN)` 明确返回 LIMIT_EXCEEDED。整数取整不经过 DOUBLE，因此 U64_MAX 不丢精度。
可用于查询、分组、窗口参数及写入表达式；CREATE/ALTER 的确定性常量默认值按已有
schema 格式保存折叠结果。未加入 DECIMAL；上述函数不隐式转换字符串/BOOL 参数。
官方依据、错误及所有权协议见[数值函数协议](design.md#单参数数值函数协议)。
支持 `ROUND(value[,digits])` 和 `TRUNCATE(value,digits)`；value 为 I64/U64/有限 DOUBLE/NULL，
digits 为 I64/U64/NULL，任一为 NULL 时返回 NULL。ROUND 省略 digits 等价于整数 0；
结果保留 value 的 kind。整数负精度使用精确十进制舍入，ROUND 中点远离零，TRUNCATE
向零，不通过 DOUBLE；结果溢出返回 LIMIT_EXCEEDED，IGNORE 也不吞掉该错误。
DOUBLE 按宿主 C 库 rint/trunc 和十进制缩放计算，正精度缩放溢出时保留原有限值，
极大负精度为零，最终非有限结果报错。小数字面量仍沿用执行器 的 DOUBLE 推断，
不代表 DECIMAL 舍入。非整数 digits 和 BOOL/TEXT/BLOB 隐式转换尚未开放。
分组、窗口外层表达式、子查询、集合、默认值及写入共享同一内核；ROUND 本身不能
加 OVER。协议及边界见[ROUND/TRUNCATE 协议](design.md#round-与-truncate-执行协议)。
支持数值 `CAST(expr AS SIGNED [INTEGER]|UNSIGNED [INTEGER]|DOUBLE [PRECISION]|REAL|FLOAT[(p)])`，REAL
采用默认 SQL mode 的 DOUBLE。接受数值、BOOL、TEXT/BLOB 参数和 NULL；整数文本
只取整数前缀，例如 `CAST('12.9e3' AS SIGNED)=12`，与列赋值得到 12900 的规则不同。
文本截断/非法/范围调整产生 1292，文本补码转换产生 1105；两个条件可以同时出现。
严格 INSERT/UPDATE/REPLACE/default 的 1292 返回 SQL_ERROR，QUERY/IGNORE 保留
结果和 warning；1105 在严格写入仍是 warning。I64/U64 数值互转保留补码且没有文本告警。
NULL 传播，转换不保留输入字节，失败不发布结果；可组合分组、窗口、子查询、集合和
有界递归 CTE。有限 DOUBLE 到整数采用执行器 的最近偶数/I64 范围 lane，
尚未保存 MySQL Item 来源或支持 DECIMAL，其他 CAST 目标也未开放。
FLOAT 无精度参数或 p=0..24 时先检查单精度范围再舍入，p=25..53 时采用 DOUBLE；
其他精度或非整数参数在绑定时返回 SQL_ERROR。单精度舍入结果通过既有 DOUBLE
载体返回，溢出在严格和 IGNORE 路径均返回 OUT_OF_RANGE，不裁剪到 FLT_MAX。
转换、诊断和兼容边界见[CAST 协议](design.md#数值-cast-执行协议)及
[FLOAT 精度协议](design.md#float-cast-精度协议)。
支持整数 `DIV`、`MOD(a,b)`、`%` 和中缀 `MOD`。二元 `+ - * / % MOD` 中任一操作数
为 DOUBLE 时，另一侧 I64/U64/DOUBLE 转为 DOUBLE 运算，结果也是 DOUBLE；原始
输入槽的 kind 仍严格校验。转换可能舍入大整数，例如 U64_MAX 与 DOUBLE 混合时
转为 2^64；纯整数取模和 DIV 不经过 DOUBLE。
整数 DIV 向零截断，任一操作数为 U64 时结果为 U64；整数取模跟随左侧 kind 和符号，
混合 I64/U64 保留精度。两个整数的 `/`、非整数 DIV 仍返回 UNSUPPORTED，暂不提供
DECIMAL；其他纯整数混合算术及 BOOL/TEXT/BLOB 算术仍未开放。集合操作的数值
结果合并见下文。所有分支仍在执行前绑定。规则及兼容边界见[混合算术协议](design.md#整数与-double-混合算术协议)。
普通比较和 `<=>` 已支持 DOUBLE 与 I64/U64 的混合比较：先转换为 DOUBLE，可能舍入
大整数；两个整数继续精确比较。列表/子查询 IN、简单 CASE 的 WHEN 和 NULLIF 复用该
规则，NULLIF 保留首参数的结果类型。BETWEEN 聚合三个数值/NULL 类型，只要任一为
DOUBLE，两次边界比较都采用 DOUBLE；参数原始 kind 仍严格校验。整数索引不将 DOUBLE
探针编码为单个整数键，沿用扫描准入和原谓词复核。类型、精度及预算边界见
[混合比较协议](design.md#double-与整数混合比较协议)。
静态 NULL 左值保持原有 NULL 传播，不对无需执行的边界值做数值转换。
小数和指数字面量仍按执行器 映射为 DOUBLE，尚未实现 MySQL 小数字面量的 DECIMAL 类型推断。
除零按固定严格模式处理：SELECT/DELETE 返回 NULL 并记录 1365 告警，严格
INSERT/UPDATE/REPLACE 及默认值折叠返回 SQL_ERROR，IGNORE 写入返回 NULL 并记录告警。
NULL/零不产生告警；EXPLAIN、LIMIT 0、懒分支及 EXISTS 的已裁剪投影不求值。
警告可通过 SHOW WARNINGS/SHOW COUNT(*) WARNINGS 查询，记录数受 sql_max_warnings
限制，完整计数保留；严格写入失败不留下部分行。类型溢出返回 LIMIT_EXCEEDED。
接入与所有权协议见[数值除法与求值上下文](design.md#数值除法取模与语句求值上下文)。
左值仅求值一次，保留 NULL 三值逻辑和闭区间语义；固定从左向右短路，编译时仍检查全部分支。
raw IN 子查询已接入下文所述依赖路径；行值与未列出的隐式类型转换尚未支持，规则见[设计第 5.2 节](design.md#52-私有-ast-谓词程序)。
[104 项表达式测试](tests/unit/expr_test.c) 验证 AST 销毁后的
执行、三值逻辑、嵌套跳转、512 层非递归表达式，以及容量和选定容器失败点的清理。
表达式模块的名称解析由调用者提供；下述 SELECT Binder 已接入单表列名解析，
TEXT 字面量支持单/双引号、成对引号、普通反斜杠转义和 document 固定的
NO_BACKSLASH_ESCAPES 模式。常量字节归程序持有，编译时验证所有分支中的 UTF-8；
空串、内嵌 NUL、尾空格均按显式长度处理。字符集前缀、相邻串拼接、COLLATE、BLOB
字面量和其他未列出的表达式仍未开放，转义依据见[官方说明](https://dev.mysql.com/doc/refman/8.4/en/string-literals.html)。
表达式 run 已改为 open 预分配、逐行复用寄存器，close 归还；活动 run 阻止程序提前
销毁，每次求值清空借用值。单次 eval 入口仍保留相同语义的临时 run 便利调用。
标量编译入口接受既有白名单的所有结果类型，谓词入口仍限制 BOOL/NULL；TEXT/BLOB
结果借用输入载荷或程序常量，调用者须保持其存活。
ASCII TEXT `LIKE / NOT LIKE` 已接入 WHERE 和计算列，支持 `%`、`_`、NULL 传播及
字面量 `ESCAPE`，例如 `SELECT id FROM items WHERE name LIKE ? ESCAPE '|'`。
默认转义遵循 document 的 NO_BACKSLASH_ESCAPES 选项。固定大小写敏感；非 ASCII、
BLOB、数字隐式转换、动态 ESCAPE 明确拒绝。匹配不分配内存，重试逐步计费；
协议及官方依据见[LIKE 增量](design.md#ascii-like内部表达式增量)。

[内部查询管线](src/scan.h) 支持 Filter、排序、槽位/计算列 Project、Offset/Limit，Filter 只接受 TRUE，
OFFSET 只计匹配行，LIMIT 0 不读取输入。schema/映射在 open 校验，扫描预算包含
被过滤和跳过的行；错误锁定首个原因，取消及完成都不再推进输入。输出借用到下一次
调用，调用者需保持源快照存活。Catalog runtime 和 relational ORM 已复用该管线；原生 iterator 行源见下文。
[20 项管线测试](tests/unit/scan_test.c) 覆盖过滤/分页、NULL、
schema 校验、取消/错误终态、预算和选定 open 分配失败；未排序且非 DISTINCT 的路径在 open 后拒绝容器分配/扩容
调用，逐行读取仍通过。尚未测得吞吐提升，不将该验证当作性能 benchmark。

[内部 SELECT Binder](src/select.h) 已将声明的单表 schema 与 MySQL SELECT AST 接入内存
查询：支持列/表别名、限定列名、星号展开、WHERE 源列解析、输出类型及常量/参数分页。
计算列支持现有算术、数值函数、谓词、CASE、COALESCE/IFNULL/NULLIF，例如
`SELECT id, score + ? AS adjusted FROM items WHERE score IS NOT NULL LIMIT 10`；
要求显式别名，输出类型和 nullability 来自表达式绑定。先过滤并跳过 OFFSET，再计算
待输出行；任何计算列失败均不发布部分行。无分组、无排序且非 DISTINCT 的路径全部工作空间在 open 分配，next 不分配。
绑定后 AST/schema 可释放，所有 run 关闭前计划保持存活；空表和 LIMIT 0 仍检查语义。
[215 项绑定及计划说明测试](tests/unit/select_test.c) 包含端到端执行、
别名作用域、重名/未知名拒绝、预算/生命周期及逐分配点失败清理。投影/WHERE/ORDER BY/LIMIT/OFFSET
以及聚合参数/HAVING 的 `?` 参数按源码顺序绑定，类型显式声明；open 复制参数值及 TEXT/BLOB 内容，各次
执行快照独立。空表、LIMIT 0 和短路分支仍验证所有参数，分页参数须为非负整数。
**MED｜兼容性边界：** 名字限区分大小写的 ASCII、最多 63 字节；拒绝重名输出、
无别名计算列、连接等尚未接入的能力。计算列各自持有独立程序，宽列表受
工作容量限制；所有权、复杂度与回滚见[计算列协议](design.md#计算列投影内部-select-增量)。详细协议与 MySQL 官方依据见
[设计第 6.2 节](design.md#62-声明-schema-的内部-select-binder)。

SELECT 支持多列 `ORDER BY ... ASC/DESC`、独立输出别名、从 1 起算的输出序号及来源列标量表达式。
非限定独立名字优先匹配输出，限定名字只匹配来源列；NULL 升序在前、降序在后，同键保持输入次序。
TEXT/BLOB 排序键、复合表达式内的输出别名和带符号数值常量键明确拒绝。
首次 next 保存全部 WHERE 匹配行及排序键，排序后应用 OFFSET/LIMIT，再计算输出表达式。
LIMIT 0 和首次 next 前取消不读数据；LIMIT 1 仍需容纳全部匹配行，受 MATERIALIZED_ROWS 上限约束。
快照、记录表增长时新旧容量峰值和排序 scratch 均计入 WORK；复制与比较计入执行步骤。
源缓冲区复用不会改变已保存的行；读取、排序键、分配或排序失败在首行发布前锁定错误。
不做截断或磁盘 spill，EOF/取消/错误后均须 close 释放资源和租约。
所有权、复杂度及官方依据见[SELECT 排序协议](design.md#select-排序执行协议)。

DISTINCT 按完整输出行去重，支持数值/BOOL/NULL，重复 NULL 合并、有限 DOUBLE 的正负零相等。
先计算全部匹配行的输出，再去重、排序及分页；输出表达式错误不能由 OFFSET 跳过。
独立别名、序号及已选表达式可用于 ORDER BY；其他排序表达式引用的来源列必须作为普通列选出。
例如 `SELECT DISTINCT score FROM items ORDER BY score DESC LIMIT 2` 可执行，
而 `SELECT DISTINCT score FROM items ORDER BY id` 返回 SQL_ERROR。
TEXT/BLOB 输出的 DISTINCT 暂不支持；没有 ORDER BY 时不承诺输出顺序。
当前复用排序物化，MATERIALIZED_ROWS 计去重前匹配行，重复率高不会降低其容量要求；
尚未实现无排序 DISTINCT LIMIT 的提前停止优化。LIMIT 0/提前取消仍不读源。
去重、排序、压紧与输出复制均计费，close 统一释放候选快照和排序记录。
所有权、资源公式及官方依据见[DISTINCT 协议](design.md#select-distinct-执行协议)。

[私有分组归约阶段](src/aggregate.h) 已实现 COUNT(*)、COUNT(value)、数值/BOOL MIN/MAX、DOUBLE SUM/AVG，
接受已计算的分组键和参数行，输出组合键及聚合结果。无键普通聚合流式归约；
DISTINCT 参数按下文保存当前组快照，有键时复用有界排序。NULL/正负零同组。
无键空输入返回一行 COUNT=0、下述位聚合的中性值或其他聚合的 NULL，有键空输入无结果。
GROUPS 计累计准入组，MATERIALIZED_ROWS 计排序候选及去重快照；EOF/取消/错误后仍须 close 释放。
下游可继续用 Scan 做过滤、投影、排序及分页，物理读只由原始来源计费。
[归约测试](tests/unit/aggregate_test.c) 覆盖数值/NULL、复合键、统计归约、
源复用、控制租约、逐分配/排序故障、所有执行步骤额度边界和取消清理。
SQL Binder 已将 GROUP BY、COUNT(*)/COUNT(expr)、MIN/MAX(expr)、DOUBLE SUM/AVG(expr) 接入该阶段，
例如 `SELECT score, COUNT(*) AS n FROM items GROUP BY score HAVING n > 1 ORDER BY n DESC`。
GROUP BY 支持现有标量表达式、独立输出别名及从 1 起的输出序号；同名时来源列优先于别名。
例如 `SELECT COALESCE(score,0) AS k, COUNT(*) AS n FROM items GROUP BY k HAVING k>0`。
WHERE、键及聚合参数引用原始行；SELECT/HAVING/ORDER BY 中普通列必须属于显式分组键。
SELECT/HAVING/ORDER BY 和窗口值/键中的表达式与分组键比较编译后的程序及输入槽，
完整或嵌套表达式相同则读取键值；不做代数改写。CEIL/CEILING 使用同一个数值 op。
HAVING 支持分组列、直接键/聚合输出别名、组键表达式和聚合调用；这些值可参与标量计算。
组结果随后执行 HAVING、投影、DISTINCT、排序和分页；空全局聚合也经过 HAVING。
AST 绑定后可释放，各阶段复制参数快照，分组及两级排序共享工作、物化与执行步骤额度。
**MED｜兼容性边界：** 暂不支持复合分组键内的输出别名、分组星号投影、其他复合输出别名的 HAVING、
嵌套聚合、下文范围之外的 DISTINCT 聚合、精确类型 SUM/AVG、ROLLUP；不推断主键函数依赖或 WHERE 单值约束。
表达式键内部的列不能单独访问；例如 GROUP BY score+1 可以计算 ABS(score+1)，
但 SELECT score 和 ABS(score) 仍拒绝，不从表达式键反推源列。
仍要求计算输出显式命名，不等同于完整 MySQL 聚合兼容。参数数目在组绑定前统一核验，
缺少参数的查询返回 SQL_ERROR，即使同时缺少计算输出别名。
所有权、分层与回滚边界见[SQL 聚合绑定协议](design.md#sql-聚合绑定首批接入协议)和
[表达式分组协议](design.md#group-by-表达式与引用接入协议)。

[私有窗口排名阶段](src/window.h) 已实现 ROW_NUMBER、RANK、DENSE_RANK、PERCENT_RANK、
CUME_DIST、NTILE：接受分区和排序列槽，携带全部输入列并追加结果列。复用 Scan 排序及
拥有型行快照，NULL/正负零是 peers；无排序时同分区全为 peers。NTILE 支持 1..2^63
桶数，不按桶数分配空间。分区/排序键限现有数值/BOOL/NULL，携带的 TEXT/BLOB 深拷贝。
首次取行完成全部计算，准备失败不输出前缀；交付步骤可能在此前成功取行后耗尽。
排序与结果快照共享 WORK/物化行/步骤限额，下游关闭前 owner 不可取消或销毁。
SELECT Binder/runtime 已接入上述六函数、LAG/LEAD、FIRST/LAST/NTH_VALUE 及 COUNT/MIN/MAX/SUM/AVG、方差/标准差的 `OVER(...)`，可在投影及最终 ORDER BY 中使用，
如 `SELECT id,ROW_NUMBER() OVER(PARTITION BY score ORDER BY id) AS n FROM items ORDER BY id`。
分区/排序键使用原始行或已归约组作用域的现有标量表达式，不能引用同层 SELECT 别名；
WINDOW 结果存入隐藏槽，星号不暴露工作列。WHERE/GROUP/HAVING 在窗口前执行，
DISTINCT/最终 ORDER/OFFSET/LIMIT 在窗口后执行。CTE、派生表、UNION、LATERAL 和
相关子查询沿用同一执行阶段和事务快照；递归 member 仍禁止窗口。
NTILE 接受 1..2^63 的整数常量或整数 marker；打开及 EXPLAIN 时验证运行值，
即使 LIMIT 0 或存在性查询剪除计算也拒绝 NULL、零、负数及超范围值。
LAG/LEAD 已支持 `expr[,N[,default]]`：默认 N=1、default=NULL；N=0 取当前行，
常量或整数 marker 的范围为 0..2^63，打开/EXPLAIN 时验证，即使 LIMIT 0 也拒绝
NULL、负数及超范围偏移。只在同一分区内寻找目标；缺行使用当前行的 default，
目标 NULL 保持 NULL。value/default 支持现有行/组标量表达式和相关子查询，
必须同类型或一方为静态 NULL；可返回数值、BOOL、TEXT/BLOB，不隐式转换。
例如 `SELECT id,LAG(score,1,0) OVER(ORDER BY id) AS previous FROM items ORDER BY id`。
TEXT/BLOB 结果引用窗口 owner 中深拷贝的目标载荷，下游仍按原协议复制/持有；
值和默认表达式在窗口前对全部保留行预求值，不承诺服务端完全相同的求值次数与错误时机。
支持 `WINDOW w AS (...)`、`OVER w` 及 `OVER(w ...)`，名称限现有 ASCII profile，
大小写不敏感且只在当前 SELECT 查询块可见。声明允许前后向继承；未知/重复名称、
循环引用、继承时新增 PARTITION BY 或重复 ORDER BY 返回 SQL_ERROR。未使用定义仍
验证名称与键类型，但不求值或增加执行窗口；继承只复用列表，不改写 AST。
例如 `SELECT id,RANK() OVER(w ORDER BY id) AS n FROM items WINDOW w AS(PARTITION BY score)`。
绑定后可释放 AST，声明图 scratch 全部归还，执行沿用现有窗口阶段和配额。
[命名窗口协议](design.md#命名窗口的-ast-与绑定边界)。
已有八函数接受 ROWS/RANGE 帧的单边或 BETWEEN 边界，按
[MySQL 帧规则](https://dev.mysql.com/doc/refman/8.4/en/window-functions-frames.html)仍使用整个分区，
帧范围不改变排名、分布或 LAG/LEAD 结果。绑定时验证边界方向和类型，打开/EXPLAIN 时
验证 marker 为非 NULL、非负值；未使用的声明、LIMIT 0 和 EXISTS 剪枝也保留验证。
ROWS 仅接受 I64/U64，RANGE 接受有限 I64/U64/DOUBLE；带数值偏移的 RANGE 要求恰好
一个数值/BOOL ORDER BY 表达式。显式帧窗口只能直接 OVER name，不能由 OVER(name)
或另一个 WINDOW 声明继承；无显式帧的父窗口仍可派生带帧子窗口。
例如 `SELECT id,LAG(id,1,0) OVER w AS previous FROM items WINDOW w AS
(ORDER BY id ROWS CURRENT ROW) ORDER BY id` 仍可取当前行之前的分区行。
帧规格由计划拥有，参数沿用快照及预算协议，绑定后可释放 AST。
FIRST_VALUE(expr)、LAST_VALUE(expr)、NTH_VALUE(expr,N) 已执行当前帧的第一、最后及第 N 行值，
保留目标 NULL，空帧或不足 N 行返回 NULL。N 必须为 1..INT64_MAX 的整数常量或 I64/U64 marker，
打开/EXPLAIN 验证，LIMIT 0 和 EXISTS 剪枝仍验证。value 接受已有行/组标量及相关子查询，
支持数值、BOOL、TEXT/BLOB；字节结果沿用上述窗口快照所有权。
N 的 NULL marker 按手册明确拒绝；8.4 源码对动态 NULL 的准入不同，详见下述取值协议。
有 ORDER BY 的默认帧为 RANGE UNBOUNDED PRECEDING 到 CURRENT ROW，含当前同值行；
无排序时覆盖整个分区。ROWS 以行位置截断；RANGE CURRENT ROW 以全部排序键识别 peers，
数值偏移以一个排序键计算，支持 ASC/DESC 和 NULL peers；同方向逆序边界允许空帧。
例如 `SELECT id,FIRST_VALUE(score) OVER(ORDER BY id ROWS 1 PRECEDING) AS first_score FROM items ORDER BY id`。
整数键与整数 RANGE 偏移按精确距离比较；DOUBLE 键或偏移使用 DOUBLE 算术。
越过数值域的比较端点只用于定位，不发布为数据值；与服务端整数边界算术的溢出报错不保证一致。
DISTINCT 的窗口等价判断包含归一化帧及参数槽身份，不能合并不同帧的取值结果。
窗口聚合已支持 COUNT(*)、COUNT(expr)、MIN/MAX(expr)、DOUBLE SUM/AVG(expr) 及下述方差/标准差和数值位聚合，
复用上述默认与显式 ROWS/RANGE 帧。COUNT(*) 计全部帧内行，COUNT(expr) 忽略 NULL；
COUNT 空帧返回 0，其他函数忽略 NULL，空帧/全 NULL 返回 NULL。
例如 `SELECT id,COUNT(*) OVER(ORDER BY id ROWS 1 PRECEDING) AS hits FROM items ORDER BY id`。
COUNT 接受已有标量类型（含 TEXT/BLOB），MIN/MAX 限数值/BOOL/NULL；SUM/AVG 限 DOUBLE/NULL，
整数参数仍明确返回需要 DECIMAL 的 UNSUPPORTED。窗口聚合保持每个来源行，不自动引入全局分组。
普通聚合可作为参数，如 `MAX(COUNT(*)) OVER()` 在 GROUP/HAVING 后读取已归约结果。
共享归约保持既有类型和中间溢出错误；AVG 只对结果副本除法，不修改缓存的累加和。
固定起点增长帧及整个分区只顺序折叠一次，COUNT(*) 直接计算帧宽度；移动起点从新帧重新累加，
以保留 DOUBLE 次序及溢出语义。额外归约状态为 O(1)，不增加 GROUPS 计费。
**MED｜资源边界：** 一般移动帧最坏 O(N²)，每个归约输入与结果均计执行步骤；宽移动帧可能
先耗尽步骤额度，返回 LIMIT_EXCEEDED，不截断或改用浮点逆向求和。预算与回滚见
[窗口聚合协议](design.md#窗口聚合与共享归约协议)。
窗口 DISTINCT 按 MySQL 窗口限制明确拒绝。
**MED｜兼容边界：** 其他窗口聚合及显式 null_treatment、FROM FIRST/LAST 未开放；
ROWS 的 INTERVAL 边界返回 SQL_ERROR；RANGE 的 INTERVAL 帧虽可解析，但当前数值表
不提供时间类型，执行返回 UNSUPPORTED；
ROWS 的 DOUBLE marker 不进行 MySQL 服务端的隐式整数转换。
帧计算范围见[取值协议](design.md#帧范围与窗口取值的执行协议)。窗口不能放在
同一 SELECT 查询块的 WHERE/GROUP/HAVING、另一个窗口键或聚合参数中；独立子查询拥有自己的作用域。
键仍限数值/BOOL/NULL，计算输出仍需显式命名。
EXPLAIN 的 Extra 标记 `Window`，不读取数据或求值窗口键，也不提供 MySQL 优化器成本估计。
[窗口算子测试](tests/unit/window_test.c) 覆盖官方样例、分区/peers、
桶数边界、链式窗口、规格与字节所有权、分页取消/租约重入、源/排序错误、
每个构造/执行分配、步骤与 WORK 字节边界。设计依据与预算协议见
[窗口执行协议](design.md#窗口排名算子的执行协议)及
[SELECT 窗口阶段](design.md#select-窗口绑定与阶段顺序)及
[LAG/LEAD 值与偏移协议](design.md#laglead-的值与偏移协议)。

SUM/AVG 仅接受 DOUBLE 或静态 NULL 参数，结果分别保持 nullable DOUBLE/NULL 类型；
忽略 NULL，空集/全 NULL 返回 NULL，AVG 分母只计非空值。可结合标量表达式、HAVING、排序和 DISTINCT。
普通 SUM/AVG 按来源顺序累加 DOUBLE，AVG 在整组完成后做除法；中间和溢出立即返回 LIMIT_EXCEEDED，
即使最终数学平均可表示也不继续计算。NaN/Inf 输入返回 TYPE_ERROR，不承诺不同物理行序逐位相同。
整数、无符号整数及 BOOL 参数返回 UNSUPPORTED，提示需要 DECIMAL；TEXT/BLOB 不隐式转数值。
例如 `SELECT SUM(score) AS total, AVG(score) AS mean FROM metrics` 要求 score 为 DOUBLE。
返回类型依据及资源协议见 [DOUBLE SUM/AVG 协议](design.md#double-sumavg-接入协议)。

普通聚合支持 `COUNT(DISTINCT expr[,expr...])`：按完整数值/BOOL/NULL 元组去重，
任一参数为 NULL 的行不计数，空集返回 nonnullable I64 的 0。
DOUBLE/NULL 的 `SUM(DISTINCT expr)`、`AVG(DISTINCT expr)` 忽略 NULL，并按升序唯一值归约；
空集/全 NULL 返回 NULL，不承诺与普通来源顺序或服务端物理计划逐位相同。
`MIN/MAX(DISTINCT expr)` 与普通 MIN/MAX 相同，不创建去重状态。
例如 `SELECT COUNT(DISTINCT score,id>0) AS combinations FROM items`。
每组仅保存所需数值参数的有界快照，组完成后释放；排序、快照、比较与归约共享
WORK/MATERIALIZED_ROWS/执行步骤额度。原分组排序和去重快照的同时存活量均计费，
超过限额立即报错，不 spill；失败不发布未完成组。TEXT/BLOB 去重、整数 SUM/AVG
所需 DECIMAL、其他函数 DISTINCT 及窗口 DISTINCT 仍未开放。
参数范围、所有权与兼容性见[普通 DISTINCT 聚合协议](design.md#普通-distinct-聚合的执行协议)。

普通分组和窗口均支持 VAR_POP/VAR_SAMP、STDDEV_POP/STDDEV_SAMP，以及 VARIANCE、STD、STDDEV
别名：VARIANCE 等于 VAR_POP，STD/STDDEV 等于 STDDEV_POP，别名大小写不敏感。
参数支持 I64/U64/DOUBLE/BOOL/NULL，返回 nullable DOUBLE；TEXT/BLOB 不隐式转换。
忽略 NULL，空集/全 NULL 返回 NULL；总体函数的单个非 NULL 样本返回 0，样本函数返回 NULL。
例如 `SELECT id,VAR_POP(score) OVER(ORDER BY id ROWS 1 PRECEDING) AS spread FROM items ORDER BY id`。
GROUP/HAVING、已有默认/显式帧、参数快照、CTE/LATERAL 与存在性剪枝沿用同一执行链。
共享在线均值/中心平方差归约避免直接平方和相减的取消误差；结果除法及 sqrt 不改写缓存状态。
统计分组新增受 WORK/PLAN_NODES 限制的固定状态槽，窗口沿用每分区固定状态和移动帧重算。
**MED｜数值边界：** I64/U64 先转 DOUBLE，超过精确整数范围的低位可能合并；不提供精确方差。
非有限中间值、负平方差和或计数溢出立即 LIMIT_EXCEEDED，即使最终数学结果可表示也不继续；
NaN/Inf 输入为 TYPE_ERROR。不保证不同物理行序或服务端浮点优化路径逐位一致；不支持统计 DISTINCT。
所有权、公式、错误与回滚见[统计归约协议](design.md#方差与标准差的共享归约协议)。

普通分组和窗口均支持 BIT_AND/BIT_OR/BIT_XOR 的数值路径，参数为 I64/U64/DOUBLE/BOOL/NULL，
结果为 nonnullable U64。忽略 NULL，空集/全 NULL/空帧的 AND 返回 UINT64_MAX，OR/XOR 返回 0。
I64 转换按模 2^64，U64 不丢最高位，BOOL 为 0/1；DOUBLE 使用 C rint 的表达式整数舍入，
默认模式为最近偶数，例如 1.5 和 2.5 都为 2，再按已检查的 I64 结果转换到 U64。
例如 `SELECT id,BIT_OR(score) OVER(ORDER BY id ROWS 1 PRECEDING) AS flags FROM items ORDER BY id`。
**MED｜兼容边界：** DOUBLE 的舍入结果超出 I64 范围返回 LIMIT_EXCEEDED，NaN/Inf 为 TYPE_ERROR；
不采用服务端越界警告后钳位。TEXT 隐式转换、BLOB 二进制归约和位聚合 DISTINCT 未开放。
不新增状态向量，沿用组/帧、分区缓存和有界步骤协议；错误不发布窗口准备结果前缀。
类型、状态归属及验证见[数值位聚合协议](design.md#数值位聚合的归约协议)。

[私有连接算子](src/join.h) 与 [FROM 执行树](src/from_run.c) 已通过 Catalog 多表来源接入 raw SQL，
支持 INNER/LEFT/RIGHT/CROSS JOIN、逗号连接、自连接和多表链。算子先按需保存右侧行，逐对执行 ON；
LEFT 无 TRUE 匹配时补一行右侧 NULL，WHERE 留在下游。候选组合（包括拒绝/UNKNOWN）
计入 JOIN_PAIRS，右侧物化、TEXT/BLOB 深拷贝和峰值扩容共享工作/物化/步骤预算。
[共享行快照](src/rows.h) 同时用于 Scan 排序和连接，不重复维护 payload 所有权。
支持 INNER/LEFT/RIGHT 的 `USING(cols)` 和 `NATURAL [INNER|LEFT|RIGHT [OUTER]] JOIN`。
依据 [MySQL JOIN 规则](https://dev.mysql.com/doc/refman/8.4/en/join.html)，公共列按普通相等
匹配，NULL 不匹配 NULL；裸名称及 `*` 只输出一次公共列，外连接使用保留侧的值。
星号顺序为公共列、第一侧独有列、第二侧独有列；RIGHT 的第一侧为右侧。
`table.col` 和 `table.*` 保留原始表值与物理列顺序，未匹配侧仍为 NULL。
无公共列的 NATURAL 保留笛卡尔积及外连接的空侧补 NULL 规则。
支持派生/CTE/LATERAL 来源和多表链，键与可见性计划均拥有独立生命周期。
**MED｜范围：** 公共列必须同 value kind，名称在每侧必须唯一；不做隐式转换或 collation
合并，不支持 FULL。重复 USING 名称、缺失列及歧义报 SQL_ERROR，类型不匹配报 TYPE_ERROR；
普通 ON 仍保留其原有输出顺序及名称消歧规则。资源计入既有 WORK/PLAN/STEPS/JOIN_PAIRS
限额，超额明确失败；[接入协议](design.md#using--natural-join-接入协议)说明所有权和兼容边界。
[22 项连接及快照测试](tests/unit/join_test.c) 覆盖重复/NULL、空侧、
ON 与 WHERE 区别、参数快照、两级连接、租约/取消、逐分配故障和全部执行步骤边界。
协议、复杂度及后续接入范围见 [M4 连接协议](design.md#m4-有界嵌套循环连接协议)。

[FROM 绑定计划](src/from.h) 已支持 INNER/LEFT/RIGHT/CROSS 表树、独立别名、自连接和 ON：
组合列保留有效表限定名，拒绝未限定同名列、重复表别名及 ON 越界引用。RIGHT 显式交换
物理输入及 ON 列槽，逻辑输出列序保持 SQL 顺序；参数按全语句源码位置编号。
`select_bind_from` 将其输出作用域接入现有 SELECT，支持限定列、`table.*`、过滤、聚合、
HAVING、排序及分页；输出名仍须唯一。重复相同 GROUP BY 键只引入一次名字绑定。
[FROM/组合 SELECT 测试](tests/unit/from_test.c) 包含 SQL AST
到内存来源 JOIN/SELECT 的执行、RIGHT 列序恢复、外连接类型传播、AST/schema 生命周期、
逐分配故障和绑定步骤限额。真实 Catalog 为每次表引用创建独立游标，包括自连接；
全部来源共享同一事务 owner，EOF/取消后仍须关闭查询才能结束事务。协议见
[FROM 名字及 ON 绑定边界](design.md#from-名字及-on-绑定边界)。

[UNION 算子](src/union.h) 与 [复合查询树](src/compound.h) 已接入 relational raw SQL。
UNION ALL 顺序拼接；UNION/UNION DISTINCT 复用现有数值排序去重，NULL 重复相等。
混合链按 AST 组合，列名来自首分支；对应列数须一致，支持同 kind、NULL-only 对齐，
以及 DOUBLE 与 I64/U64 的共同 DOUBLE 结果。转换先于排序、去重及重复计数；
大整数可能舍入为同一值，纯整数集合继续精确比较。
同一查询表达式汇总全部查询块的 kind，晚出现 DOUBLE 也影响前面的去重；
不同集合操作、左右括号及带排序/分页的组共用 kind，仍按原 AST 执行集合与分页。
派生表、独立子查询和 CTE 定义由各自查询表达式决定类型。
结果 nullable：UNION 取两侧并集，INTERSECT 取交集，EXCEPT 仅取左侧。
原始输入 nullable/kind 校验保持独立，不能以结果约束替代输入约束。
`(SELECT ... ORDER BY ... LIMIT ...) UNION ALL ... ORDER BY ... LIMIT ...` 的内外层分页分别执行。
参数统一按整个语句位置编号，分支可含已支持的 JOIN/聚合，全部使用同一事务快照。
ALL 支持 TEXT/BLOB 参数输出；DISTINCT 仍限数值/BOOL/NULL。无 DOUBLE 的 I64/U64
混合需要 DECIMAL，其他异种非 NULL 类型仍拒绝。
**MED｜范围：** TABLE/VALUES 集合项尚未开放；无 FROM 查询及 CTE 范围见下文。
多次物化共享同一额度，无磁盘 spill。
详细生命周期、失败清理及兼容性见 [UNION 协议](design.md#m4-union-拼接与去重协议)
与[数值集合结果协议](design.md#集合操作数值结果转换协议)。
[33 项集合算子测试](tests/unit/union_test.c) 包含六种模式、
精度边界、多列转换排序、原始输入校验及逐分配/排序/步骤失败。

INTERSECT/EXCEPT 已接入 relational raw SQL、依赖查询、相关查询、CTE、派生来源和
LATERAL，并支持 ALL、默认/显式 DISTINCT 及 MySQL 的 INTERSECT 优先级。
数值/BOOL/NULL 完整元组按两侧有界排序归并；ALL 分别保留 min(left,right) 和
max(left-right,0) 次，DISTINCT 先对两侧分别去重。EXISTS 仍计算真实元组，完成集合
比较后才产生 witness；分页应用于集合结果。EXPLAIN 输出具体分支和结果标签，
不读取业务行或求值表达式。递归 CTE 的纯初始子树可使用集合操作，含递归引用的
节点仍要求 UNION。DOUBLE/整数转换与 UNION 共用规则；TEXT/BLOB、collation 及其他
异种非 NULL 类型转换继续明确拒绝。数值共同 kind 已覆盖右侧嵌套、混合操作和
带尾部的组；这不代表全部 MySQL 类型、collation 或服务端优化行为等价。
协议见[集合查询接入](design.md#集合操作接入复合查询的执行协议)。

[私有 Catalog 定义绑定](src/catalog.h) 将单条 MySQL CREATE TABLE 转换为独立拥有的
schema，可供 SELECT bind 和 table_open 复制使用；销毁 AST/定义后查询仍可执行。
例如 `CREATE TABLE metrics (id BIGINT PRIMARY KEY, score BIGINT NOT NULL)`。
首批类型为 BIGINT、BIGINT UNSIGNED、DOUBLE，支持 NULL/NOT NULL、常量表达式经严格赋值
转换后的数值/NULL 默认值、列级/表级单列整数主键及 IF NOT EXISTS 意图；主键隐含非空并可使用兼容的
数值默认值。默认表达式在建表时折叠成 Catalog 标量，不保存 AST，也不在插入时重新求值。
没有主键或未能执行的类型、约束、表选项一律拒绝，包含 TEXT/BLOB、动态默认表达式、
AUTO_INCREMENT、外键与 CHECK。
这一步只校验和拥有元数据，不创建表，不检查表是否存在，也不写入 Catalog。
[39 项定义及 codec 测试](tests/unit/catalog_test.c) 验证类型/主键、
名称和错误边界、资源限制、逐分配点退款及生命周期。协议及官方依据见
[Catalog 定义增量](design.md#catalog-建表定义私有绑定增量)。

[私有持久化 Catalog](src/catalog_store.h) 已实现独立空 SYNC_FULL CF 的显式初始化、
版本化 Manifest、事务内建表及按表名读取 schema/表 ID/写版本。目录、初始表版本与
ID 分配状态在同一事务提交；每条 CREATE 用私有保存点保护，失败不遗留半张表。
IF NOT EXISTS 返回已有表，不覆盖定义；普通重名报 CONSTRAINT。回滚/保存点清理失败
禁止继续提交；提交冲突报 BUSY，其他提交失败报 COMMIT_UNKNOWN，不自动重试。
新 CF 由调用者创建并独占初始化；不会自动识别、接管或迁移已有 legacy 数据。

[39 项目录/SHOW 集成测试](tests/integration/catalog_store_test.c)
覆盖重开、空表 SELECT 绑定、快照、同名/异名创建冲突、整事务及逐写入点回滚、
提交不确定、格式损坏/孤立版本、初始化/SYNC_FULL 拒绝、容量和分配/native get 故障。
schema codec 有独立 golden bytes、每个截断长度与坏字段测试，目录 key/entry/version
也有精确字节断言。**HIGH｜接入边界：** 当前目录与下述新关系行共用独立 CF，
尚无生产 SQL 路由；CREATE 事务语义也不等同于 MySQL DDL 隐式提交。
[完整协议](design.md#私有持久化-catalog-协议)。

[私有关系行模块](src/relation.h) 已支持按目录表名插入完整类型化行，以及打开供 SELECT
使用的原生扫描源。类型为 I64/U64/有限 F64，非主键可按 schema 允许 NULL；无隐式转换。
Data key/value 使用独立版本化编码，主键与行内值一致，插入与 TableVersion 递增使用
同一保存点批次。扫描打开时登记表版本，活动 source 阻止同 owner 写入和结束事务；
EOF/取消后仍须 close。

[105 项关系行集成测试](tests/integration/relation_test.c) 覆盖重开后
SQL SELECT、数值端点/NULL/负零、精确 wire bytes、主键重复/并发冲突、部分写入与
整事务回滚、空范围读后写偏差检测、表隔离、损坏行、资源/原生故障和源的生命周期。
[私有 SQL INSERT 入口](src/insert.h) 支持 MySQL `INSERT INTO t(c1,c2) VALUES (...),(...)`、
`INSERT INTO t SET c1=...,c2=...` 和 `INSERT INTO t(c1,c2) SELECT ...`。VALUES/SELECT
列清单可省略、显式为空、部分指定或重排，SET 可部分赋值；SELECT 支持已有 SELECT、UNION、括号查询组和非递归 WITH runtime，
包括读取目标表。参数按源码顺序绑定，三种形式都物化为 schema 顺序候选行。
整批行与一次 TableVersion 递增共享保存点；任一行失败不留下部分写入，affected 仅成功时更新。
类型化行与编码行分别计入物化预算，R 行的峰值为 2R；工作空间和读写预算也先行约束。
所有固定向量分配故障、每个 native put 失败、保存点清理失败、提交重开均有正式测试。

MySQL `VALUES/SET ... ON DUPLICATE KEY UPDATE` 已接入 `orm_raw()`：逐候选行先查主键，
再按目录顺序查非 NULL 唯一索引；赋值从左到右读取当前行，`VALUES(col)` 读取本次候选行。
参数沿用源码顺序。affected 采用 MySQL 默认连接语义：插入为 1、实际更新为 2、无变化为 0。
整条命令使用外层私有保存点；多行执行中任一约束、求值或存储错误会撤销本条命令已完成的行，
显式事务中更早成功的语句仍保留。

`INSERT IGNORE ... VALUES/SET/SELECT` 会跳过主键和非 NULL 唯一索引冲突并继续后续候选，affected
只统计实际插入行。与 `ON DUPLICATE KEY UPDATE` 组合时，赋值结果造成的键冲突也跳过该候选。
被忽略候选不递增 TableVersion；其他求值、类型、资源或存储错误仍回滚整条命令。

INSERT SELECT 在写入前完整物化有界查询结果并关闭其全部 source，随后在外层命令保存点中
按结果顺序写入；因此同表读取不会与写入租约冲突，后续候选失败也会撤销前面已写候选。
查询输出列数及每个实际值的类型/NULL 约束必须与目标列严格匹配；空结果成功且 affected 为 0。

**HIGH｜接入边界：** 省略列按显式 schema 默认值填充；没有显式默认值的可空列使用 NULL，
NOT NULL 列则在写入前返回 SQL_ERROR。裸 DEFAULT 可用于 VALUES、SET 和重复键赋值。
CREATE、ALTER SET DEFAULT 和 ADD COLUMN 默认值支持确定性常量表达式的数值、BOOL、
数值文本与 NULL，先折叠再转换为列类型。BIGINT 小数按半远离零舍入；字符串支持
精确十进制小数/指数，不经过 DOUBLE 中转。无效文本返回 TYPE_ERROR，越界返回
OUT_OF_RANGE，NOT NULL 默认 NULL 仍返回 SQL_ERROR，失败保留原定义/默认值。
DOUBLE 数值文本总是生成有限 DOUBLE。动态函数及引用列、参数或子查询的默认表达式仍不支持。
配置正数 `sql_max_recursive_iterations` 后，递归 WITH INSERT SELECT 使用同一有界 CTE runtime；
`sql_client_found_rows=true` 使 UPDATE 返回匹配行数，并使无变化的重复键更新从 0 返回 1。
VALUES/SET 支持 `AS row_alias [(column_alias,...)]`，重复键赋值可通过限定行别名或唯一的
非限定列别名读取不可变候选行；普通非限定目标列名仍读取当前行。数值目标列支持 BOOL、数值、
TEXT/BLOB 数值文本的 MySQL 赋值转换；整数目标支持完整十进制小数/指数并按半远离零舍入。
严格模式拒绝非空白尾缀，IGNORE 转换数值前缀并记录既有告警。`'12.9e3'` 现在转换为 12900，
`'2.5'` 转换为 3；这修正了旧整数前缀截断行为。
`REPLACE` 支持 VALUES、SET 和 SELECT，
并按删除的冲突行数加插入行数报告影响行数。
IGNORE 会把 NULL 到 NOT NULL、无效数值和越界数值调整为目标列的零值或最近合法边界，
同时记录 1048/1264/1265/1366 告警；被忽略的主键/唯一键冲突记录 1062。
`SHOW WARNINGS [LIMIT ...]` 返回 Level/Code/Message，`SHOW COUNT(*) WARNINGS` 返回完整条件数。
告警属于连接，后续非诊断语句会重置；`sql_max_warnings` 只限制保留行数。
AST 通过 `columns_specified` 区分省略清单和显式空清单；列引用报 SQL_ERROR。
若一个候选行同时冲突多个唯一约束，当前按索引目录顺序选择首个 owner；调用方不应依赖其与
MySQL 物理索引顺序一致。
本批语法与多行规则参考 [MySQL INSERT 官方说明](https://dev.mysql.com/doc/refman/8.4/en/insert.html)，
重复键语义参考 [MySQL ON DUPLICATE KEY UPDATE 官方说明](https://dev.mysql.com/doc/refman/8.4/en/insert-on-duplicate.html)，
完整调用见上述集成测试中的 `SQL INSERT VALUES/SET`、`INSERT SELECT`、`MySQL duplicate-key` 与 `INSERT IGNORE` 分组。
[协议与格式](design.md#私有关系行插入与扫描协议)。

[私有 UPDATE/DELETE 入口](src/change.h) 已支持单表可选 WHERE、参数、含主键的多项表达式赋值及
`UPDATE ... SET column=DEFAULT`，
以及整表/条件删除。赋值按从左到右执行，可引用前面更新后的列；WHERE 始终读取原行。
数值目标列使用与 INSERT 相同的 MySQL 赋值转换；普通 UPDATE 拒绝非法、越界或 NULL 到非空列，
`UPDATE IGNORE` 调整为最近合法值并写入连接诊断区。
affected 统计实际改变/删除行数，无变化和无匹配返回 0，且不递增表版本。
先在同一快照计数，再物化改动，关闭所有源后提交单个 put/delete 保存点批次。
任何扫描、求值或资源错误都发生在写入之前；部分写入失败回滚，清理失败要求整事务回滚。
支持 `LIMIT row_count`，接受非负整数字面量或 I64/U64 参数；UPDATE 按匹配行计数，
无变化行也消耗上限。`LIMIT 0` 仍绑定名称/类型/参数，但不读取 Data 行；无排序时达到上限后不再拉取下一行。
支持 `ORDER BY col [ASC|DESC], ...`，包含表名限定列、多列方向和 NULL 排序（升序在前，降序在后）。
排序项也支持现有标量表达式白名单中的算术、CASE、COALESCE/IFNULL/NULLIF、比较和参数，
结果限 I64/U64/有限 F64/BOOL/NULL；布尔值按 false、true 升序排列，不做隐式类型转换。
有排序时复制全部匹配行，按原始列值计算排序键，排序后应用 LIMIT，再执行赋值；修改排序列不会改变本次行选择。
计算键每个匹配行只求值并保存一次，不在比较器内执行表达式；任一候选键出错都使语句零写入失败。
M 个匹配行、K 个被选行需要 O(M*C) 数值快照及 O(M) 排序记录/scratch，物化行峰值不超过 M+2K。
E 个计算排序项额外需要 O(M*E) 标量键工作区，纯列排序不分配此矩阵；所有空间和求值受现有预算约束。
TEXT/BLOB 排序、裸数值/直接带符号数值排序项、写入 LIMIT 的 OFFSET/逗号形式、多表和优先级仍明确拒绝；
无 ORDER BY 或排序键完全相同的行不承诺选择顺序。

主键赋值使用同一数值转换与非空约束，后续 SET 表达式可读取转换后的新主键。
原始候选行只处理一次；按选定顺序检查目标键，拒绝未腾出的键、循环交换、重复目标和覆盖存量行。
例如连续键的 `UPDATE items SET id=id+1 ORDER BY id DESC` 可以成功；升序遇到占用目标返回 CONSTRAINT。
旧键删除、新行写入和一次版本递增共用保存点；R 个实际改变行最多产生 2R+1 次原生写入。
没有修改主键的语句保持原批次路径；完全无变化不写版本，失败保留 affected 和语句之前的数据。
`UPDATE IGNORE` 在外层命令保存点内按选定顺序逐行写入；主键或唯一索引冲突只跳过当前行并记录 1062，
其他资源、求值或存储错误回滚本语句已经完成的行。affected 只统计实际写入行。

SELECT 与 UPDATE/DELETE 共用 [表达式绑定器](src/binding.h) 的名字解析和迭代 AST 遍历，
三个 SQL 入口共用参数位置索引；编译后的程序与寄存器在扫描前准备，不逐行重编译。
新增 18 项 UPDATE/DELETE 集成用例覆盖赋值顺序、NULL、无变化、空表验证、故障回滚、
分配/配额、两遍扫描、并发版本读集和提交重开；实际调用见关系行测试对应分组。
另有 9 项 LIMIT 用例覆盖匹配计数、参数顺序、零/最大整数、读取边界和限量批次故障回滚，
既有固定向量分配故障测试也包含 LIMIT 参数路径。资源配额不足使整条语句失败，不缩减 LIMIT。
另有 13 项 ORDER BY 用例覆盖原始值排序、多列/NULL/数值端点、排序后 LIMIT、原生批次顺序、
排序/第二遍扫描故障、物化配额及每次写入失败回滚；固定向量分配故障遍历也覆盖有序 UPDATE/DELETE。
再增加 9 项计算排序键用例，覆盖跨语句子句参数顺序、独立键生命周期、函数/布尔/数值键、
候选溢出与 WHERE/CASE 短路、LIMIT 0 绑定、排序失败重试和写入回滚；分配故障遍历包含计算键路径。
主键更新增加 13 项用例，覆盖顺序冲突、重复赋值/无变化、LIMIT、非首列主键、整数端点、
逐次 delete/put 故障、保存点清理失败、旧键索引排序失败、写预算边界、损坏记录、并发竞争和提交重开。
规则来源：[MySQL UPDATE](https://dev.mysql.com/doc/refman/8.4/en/update.html)、
[MySQL DELETE](https://dev.mysql.com/doc/refman/8.4/en/delete.html)、
[MySQL NULL 排序](https://dev.mysql.com/doc/refman/8.4/en/working-with-null.html)。


[私有 SHOW 结果源](src/show.h) 支持 `SHOW [FULL] TABLES`、
`SHOW COLUMNS/FIELDS FROM table`、`SHOW INDEX/INDEXES/KEYS FROM table`（FROM 可用 IN）、
`SHOW WARNINGS [LIMIT ...]` 和 `SHOW COUNT(*) WARNINGS`，
从同一 Catalog 事务快照读取表名、数值列定义和索引目录。
结果列按照 [MySQL SHOW TABLES](https://dev.mysql.com/doc/refman/8.4/en/show-tables.html) 与
[SHOW COLUMNS](https://dev.mysql.com/doc/refman/8.4/en/show-columns.html) 的布局提供；
FULL TABLES 增加 `Table_type`，COLUMNS 返回 Field/Type/Null/Key/Default/Extra。
Default 对显式整数默认值返回规范化文本，对显式 NULL 或无默认值返回 SQL NULL。
Key 优先级 PRI > UNI > MUL：主键为 PRI，单列唯一索引首列为 UNI，普通或复合
唯一索引首列为 MUL，后续键列不单独标记；删索引后自动反映当前目录。
INDEX 使用 [MySQL SHOW INDEX](https://dev.mysql.com/doc/refman/8.4/en/show-index.html)
的 15 列布局，含 PRIMARY，每个索引列一行，序号从 1 开始，方向为 A/D。
**MED｜兼容性边界：** Index_type 为本地 LSM，Cardinality 尚无统计而返回 NULL；
Sub_part/Packed/Expression 为 NULL，Visible 为 YES，注释为空。不承诺索引枚举顺序。
数据源可交给现有 scan，支持其投影、LIMIT/OFFSET、取消与关闭协议。TABLES 流式
读取表目录；COLUMNS/INDEX 在 open 内持有本表已验证的有界索引快照，next 不读存储。
TABLES/COLUMNS/VARIABLES 支持 LIKE 或 WHERE，INDEX 支持 WHERE。WHERE 引用
实际显示的结果列名，例如 `Tables_in_rel`、`Field`、`Non_unique`、`Variable_name`，
支持既有标量表达式和类型化参数；关键字列名须用反引号引用，例如 `Null`/`Collation`。
SHOW 输出列名按 ASCII 不区分大小写绑定，普通表列绑定保持既有规则。
LIKE 的表名匹配大小写敏感，列名和变量名匹配采用 ASCII 折叠；WHERE 的值比较
沿用现有表达式规则，不承诺完整 MySQL collation。WHERE 的子查询/聚合明确拒绝。
过滤逐行执行，AST 和参数输入在 open 后可释放，取消、步骤限额和失败清理共用
现有 Scan 协议。详见 [过滤归属](design.md#show-过滤的执行归属与生命周期)。
数据库限定、FULL/EXTENDED COLUMNS、EXTENDED INDEX 和未列出的 SHOW 仍明确拒绝。
这不改变数值表的存储类型；SHOW 现已通过 关系执行器 的 `orm_raw()` 查询入口接入。
新增 16 项 Catalog/SHOW 集成用例覆盖结果、快照、持久化、生命周期、损坏数据、
iterator/get/分配故障、资源限额和显示名称所有权。

`SHOW CREATE TABLE table` 返回 Table/Create Table 两列单行结果。按照
[MySQL SHOW CREATE TABLE](https://dev.mysql.com/doc/refman/8.4/en/show-create-table.html)
生成规范化定义，保留列序、三种数值类型、NULL/NOT NULL、显式列默认值、主键及完整二级索引的
名称、唯一性、复合列序与 ASC/DESC；引用全部名称，不补造 ENGINE 等属性。
生成文本在 open 内按 WORK/STEP 预算构建，读取与释放沿用上述快照和租约协议。
生成 SQL 可在同一 关系执行器 中重建数值表和全部具名二级索引，目标表名
须不存在；不会导出表内数据。数据库限定仍不支持。表级匿名索引及列级 UNIQUE
使用首列名并以 `_2`、`_3` 消重；显式名称优先，结果不受定义顺序影响。
SHOW CREATE 会输出生成后的显式名称。回放后唯一约束、索引查询、用户保存点和
重开测试见 relational/index_store 集成用例。

[私有 EXPLAIN 结果源](src/explain.h) 支持 `EXPLAIN [FORMAT=TRADITIONAL] SELECT ...`，
输出 id/select_type/table/partitions/type/possible_keys/key/key_len/ref/rows/filtered/Extra。
先按同一 Catalog 快照绑定完整 SELECT 并验证参数，展示当前全表扫描及过滤、聚合、排序等步骤。
未知 rows/filtered 估算返回 NULL；LIMIT 0 显示 Zero limit、type=NULL、rows=0。
`Using temporary` / `Using filesort` 对应当前有界内存物化和稳定排序。
目录读取照常计入预算；说明结果为派生行，不创建业务行 iterator，不执行 SELECT 表达式。
说明值独立拥有，AST/参数可在 open 后释放；查询仍占用事务租约，EOF/取消后须 close。
JOIN EXPLAIN 每个表引用输出一行，按实际嵌套循环顺序展示有效别名；Extra 的
`Nested loop ... JOIN`、`RIGHT JOIN via LEFT`、`Materialized input` 是本执行器的说明，
不承诺与 MySQL 优化器选路相同。RIGHT 的物理输入交换不会改变查询结果的 SQL 列序。
复合查询 EXPLAIN 支持 UNION ALL/DISTINCT、混合链和括号查询：先列出孩子，再列出合并/分页节点，
SELECT 按语句顺序编号，同一 JOIN 分支共享编号。`UNION RESULT` 的 id 为 NULL；
`QUERY GROUP`、`union_result`/`query_group` 表标签及 Extra 中的算子文字属于本执行器。
计划仍校验所有分支、参数和全局排序分页，但不打开业务执行器、不消费业务行或 JOIN 配对。

**MED｜边界：** 仅默认/TRADITIONAL，JSON/TREE、ANALYZE 和 DML EXPLAIN 暂不支持；
原 SELECT 不支持的语义也不会通过 EXPLAIN 放行。实现契约及官方依据见[设计文档](design.md#只读-explain-接入协议)。

[私有统一 runtime](src/runtime.h) 已连接 CREATE TABLE、INSERT、UPDATE、DELETE 命令，
以及 SELECT/SHOW/EXPLAIN 查询。查询共用列元数据、next/cancel/close 接口，自动按 Catalog
查找 schema；AST 和参数在 open 后可释放，返回行借用到下次 next/cancel/close。
命令在现有 owner 事务内执行，CREATE affected 为 0，DML 沿用实际影响行数；不会隐式提交。
数据库显示名只用于 SHOW，不参与 CF 选择。runtime 拒绝 SQL 事务命令，保存点与
连接级事务生命周期/特征命令由 relational backend 处理；批量语句、未覆盖语法和错误入口均拒绝。
[218 项 runtime 集成测试](tests/integration/runtime_test.c) 覆盖事务内连续执行、
显式提交重开、整体回滚、参数所有权、双查询租约、构造失败清理、预算/分配失败和持久化行损坏。
逐语句预算已支持：Catalog owner 的常驻元数据占用共享 WORK_BYTES 中的 retained 子集，
查询、source、schema 和临时工作仍须在 `budget_end` 前释放。下一次 `budget_begin`
清空语句计数，以 retained 工作量为内存起点，保留事务累计读写额度和同一原生快照。
事务可在语句间隙提交/回滚；owner 结束前 `reset_transaction` 返回 BUSY，不能绕过累计限额。
runtime 不隐式开始/结束预算，调用方显式设置语句边界；错误、EOF 和取消后的查询仍须 close。
新增 6 项预算、7 项 runtime 和 3 项 Catalog 用例验证跨语句容量、限额、所有权与提交错误清理。
**MED｜接入边界：** raw SQL、显式事务、物化结果和 Publisher 已接入 关系执行器；
用户 savepoint 与结构化 ORM CRUD 已接入，M2 数值表最小闭环已接通；
SELECT 排序、数值 DISTINCT、GROUP BY/HAVING、COUNT/MIN/MAX、DOUBLE SUM/AVG 及基本 JOIN 已接入。
USING/NATURAL JOIN、派生表、相关子查询、集合操作及索引读取已接入，
具体范围见对应段落。重复输出列名仍要求显式别名。
[真实插件测试](../orm/tests/integration/tidesdb/sql/relational_test.c) 验证连接、执行、排序/去重/聚合、计划说明及保存点闭环；
[40 项 owner 故障测试](../orm/tests/integration/tidesdb/sql/relational_owner_test.c)
验证提交冲突、COMMIT_UNKNOWN、回滚错误、分配失败和 Catalog 读取失败，
以及保存点原生调用前后故障、私有批次隔离、注册表释放和工作/执行步骤限额，
结构化渲染的参数分配/每次文本追加故障、执行参数预算失败、危险命令修饰拒绝。
SQL 保存点覆盖与 ORM 混用、自动提交不保留点、大小写/引用名称、容量与取消准入、
失败后状态、目录/索引/默认值回滚与重开，以及 native 各阶段前后故障。
连接级 SQL 事务另覆盖 READ ONLY、CHAIN、重复 BEGIN 的隐式提交、raw/结构化共享快照、
Publisher 与连接延迟释放、提交冲突/未知结果、新 owner 分配失败及最终回滚失败的子进程验证。
事务特征另覆盖 SESSION/LOCAL 与下一事务覆盖、SQL/ORM 入口、自动事务消费、活动模式冻结、
设置不随回滚撤销、CHAIN/再次 BEGIN 保留模式、取消准入及参数/预算/分配/失败 owner 拒绝。
autocommit 另覆盖同值无提交、隐式事务延迟创建/共享、关闭后持续事务、值与作用域校验、
只读/ORM/保存点/cursor/取消/重开，以及提交冲突/未知结果、创建/查询分配失败和表达式工作区退款。
另有 5 项通用 SQLite owner 测试
验证物化执行也持有 native admission 和查询/事务租约，阻止同连接重入并延迟句柄清理。

事实｜Windows Release DISTINCT 增量本轮验证 17 个 TidesDB CTest 目标，全部通过，耗时 59.78 秒，包含
32 项 relational 插件、17 项 relational owner 故障测试，以及 select 86、scan 18、expr 64、value 26、
测试覆盖预算、Catalog、关系 CRUD、runtime、原生存储/WAL 和结果所有权。
前轮结构化入口验证另有 5 项共享 MySQL 渲染测试通过；更早接入验证有 8 个 ORM owner 与 4 个公共流/后端/SQLite 集成目标通过（checked owner 含 185 项）。
未运行 MySQL 服务端差分或 sanitizer。

事实｜随后分组归约基础阶段验证 6 个相关目标通过，耗时 14.62 秒：aggregate 25 项/5324 条断言、
expr 64、scan 18、select 86、runtime 23、真实 relational 插件 32 项。此轮未新增 SQL 语法准入。

事实｜SQL 聚合绑定增量的 18 个 TidesDB CTest 目标全部通过，耗时 61.26 秒。
SELECT 99 项/10353 条断言，expr 65 项/5518 条断言，真实插件 35 项/1493 条断言，
aggregate 25 项/5324 条断言；构建无编译警告或错误，git diff --check 通过。
尚未运行 MySQL 服务端差分、sanitizer 或性能 benchmark。

事实｜GROUP BY 表达式/引用增量的 18 个 TidesDB CTest 目标全部通过，耗时 61.28 秒。
SELECT 108 项/12221 条断言、expr 66 项/5538 条断言、真实插件 37 项/1610 条断言；
包含键表达式、别名/序号解析、参数身份、临时比较程序分配失败与保存点快照。
构建无警告或错误，git diff --check 通过；未运行 MySQL 服务端差分、sanitizer 或 benchmark。

事实｜只读 EXPLAIN 增量的 18 个 TidesDB CTest 目标全部通过，耗时 64.37 秒。
SELECT/说明源 110 项/12268 条断言、runtime 27 项/2568 条断言、真实插件 39 项/1727 条断言；
构建无警告或错误，git diff --check 通过。未运行 MySQL 服务端差分、sanitizer 或 benchmark。

事实｜DOUBLE SUM/AVG 增量新增 16 项正式用例，18 个相关 TidesDB CTest 目标全部通过，
耗时 66.28 秒。aggregate 32 项/6143 条断言、SELECT 116 项/12546 条断言、真实插件
42 项/1916 条断言；覆盖浮点溢出、NULL 分母、逐分配故障、执行步骤限额、参数快照、
保存点回滚及重开。构建无警告或错误，git diff --check 通过；未运行 MySQL 服务端差分、sanitizer 或 benchmark。

事实｜M4 私有连接/共享快照增量的 19 个相关 TidesDB CTest 目标全部通过，耗时 66.30 秒。
新增连接/快照测试 22 项、4719 条断言通过；构建无警告或错误，git diff --check 通过。
未运行 MySQL 服务端差分、sanitizer 或 benchmark；本轮不宣称 SQL JOIN 已开放。

事实｜FROM/组合 SELECT 增量新增 22 项测试、2488 条断言；20 个相关 TidesDB CTest 目标全部
通过，耗时 64.92 秒。既有 SELECT 116 项/12546 条断言、JOIN 22 项/4719 条断言回归通过。
构建无警告或错误，git diff --check 通过；未运行 MySQL 服务端差分、sanitizer 或 benchmark。

事实｜Catalog 多表 JOIN 增量新增 16 项正式用例，Windows Release 的 20 个相关 CTest 目标全部通过，耗时 72.73 秒。FROM 25 项/2713 条断言、runtime 35 项/8586 条断言、真实 relational 插件 47 项/2145 条断言；既有 JOIN 22 项及 SELECT 116 项回归通过。构建无编译警告或错误，git diff --check 通过；未运行 MySQL 服务端差分、sanitizer 或性能 benchmark。

事实｜UNION/复合查询增量的 Windows Release 验证：21 个 TidesDB CTest 目标及共享 SQLite 行流目标，共 22 个目标全部通过，耗时 84.24 秒。UNION 算子 15 项/3495 条断言、runtime 44 项/12633 条断言、真实 relational 插件 52 项/2356 条断言、共享行流 19 项/211 条断言。既有 SELECT 116 项及 FROM 25 项回归通过。构建无编译警告或错误，git diff --check 通过；未运行 MySQL 服务端差分、sanitizer 或性能 benchmark。

Windows 在 MSVC 开发环境中运行，依赖根沿用已有 preset：

```powershell
cmake --build --preset win-release-user --target orm_tidesdb_sql_budget_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_value_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_expr_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_scan_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_aggregate_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_join_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_union_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_from_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_select_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_catalog_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_catalog_store_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_relation_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_runtime_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_relational_test orm_tidesdb_sql_relational_owner_test
cmake --build --preset win-release-user --target orm_mysql_dialect_test orm_mysql_driver
cmake --build --preset win-release-user --target orm_tidesdb_sql_storage_test orm_tidesdb_sql_wal_fault_test
cmake --build --preset win-release-user --target orm_tidesdb_sql_storage_race_test
ctest --preset win-release-user -R '^orm_tidesdb_'
ctest --preset win-release-user -R '^orm_mysql_dialect$'
```

Linux 使用 `ci-linux-release` 的同名 targets 和测试过滤。常规 CI 自动执行这些 CTest 用例，不增加独立 workflow。

事实｜UNION EXPLAIN 增量的 Windows Release 验证：21 个 TidesDB CTest 目标全部通过，耗时 79.47 秒。runtime 49 项/25373 条断言、真实 relational 插件 54 项/2492 条断言，UNION 算子 15 项/3495 条断言。本轮新增 7 个用例，包含逐分配故障与每个取行步骤边界；既有 SELECT/FROM/JOIN、存储和事务回归通过。构建无编译警告或错误，git diff --check 及新增文件空白检查通过。未运行 MySQL 服务端差分、sanitizer 或性能 benchmark。


[私有子查询求值器](src/subquery.h) 实现 SCALAR、EXISTS/NOT EXISTS、IN/NOT IN 的基数检查和三值逻辑，
复用 Scan/Rows 与既有比较规则。raw SQL 开放非相关子查询及受限的直接相关子查询：

- 支持 SELECT、UNION 和括号查询；标量/IN 要求单列，EXISTS 接受多列及星号。嵌套依赖共享同一 Catalog 事务快照。
- 内层计算列可省略别名；有 FROM 的顶层计算列仍须显式别名。无 FROM 的输出标签规则见下文。
- 标量零行返回 NULL；超过一行在实际求值时返回 SQL_ERROR。CASE、AND/OR、COALESCE/IFNULL
  跳过的分支不执行子查询；名称、类型和参数错误仍在打开时拒绝。
- IN 首次到达时有界物化集合一次，后续左值复用缓存；命中优先于 NULL，空集的 IN 为 FALSE、
  NOT IN 为 TRUE（包括左值 NULL）。左值与元素类型在编译阶段验证，不做隐式转换。
  按 MySQL 限制拒绝 IN 查询块中的 LIMIT，内嵌标量查询自身的 LIMIT 不受此限制。
- EXISTS 首次求值只观察一行并缓存非空 BOOL；NOT EXISTS 反转结果。跳过无用投影、排序和聚合参数，
  保留 WHERE、分组、HAVING、LIMIT/OFFSET，以及影响分页行数的 DISTINCT/UNION 去重。
- WHERE、JOIN ON、组键、聚合参数、HAVING、投影和排序使用统一来源映射。
  AST 和参数载荷可在打开后释放；标量结果按语句缓存，字节载荷由缓存拥有。
- EXPLAIN 输出 PRIMARY/SUBQUERY/UNION 查询块及复合结果节点，不执行业务表达式或基数检查。
  打开时读取 Catalog 元数据并计入预算，取 EXPLAIN 行不读取业务行。
- 构造按内层到外层打开，关闭按反序释放；EOF/取消后保留租约至关闭。
  构造深度、计划、工作区和步骤使用已有额度，失败退还工作区及来源租约。
- 对普通单表或仅含普通表 JOIN 的外层 SELECT，以及单表 UPDATE/DELETE，直接
  scalar/IN/NOT IN/EXISTS/NOT EXISTS 子查询可用 `外层别名.列` 引用当前行。写语句声明目标别名时
  只暴露别名，否则暴露目标表名。内层局部列优先，同名内层限定符遮蔽外层；相关依赖按外层行重开并在
  本次同步求值后关闭，不复用非相关缓存。未限定名先在内层 FROM schema 查找，找不到时再查外层列；
  JOIN ON、WHERE、分组、投影和排序均可消费相关依赖；LEFT/RIGHT JOIN
  传入已经完成 NULL 扩展的逻辑行。内层可为无 FROM、单个普通表或仅含普通表的 JOIN；内层 JOIN
  的 ON 及其余 SELECT 表达式都可显式引用外层行，并为每个外层行重开独立来源游标。直接子查询还可
  包含一层或多层派生表或非递归 CTE；其定义查询可捕获该直接子查询所见的外层行。运行时按由内到外
  顺序重开查询来源；相关 CTE 每行只物化一次，同一行内的多个 occurrence 使用独立 reader 共享该结果。
  命名 CTE 作为根 SELECT 行来源或根 JOIN 输入时，其 reader/schema 会先于顶层表达式依赖发布；
  JOIN 在全部输入就绪后生成带来源限定符的组合 schema，因此直接 scalar/IN/EXISTS 可逐行引用任意
  JOIN 输入列。每个 occurrence 仍拥有独立 reader，定义仍只物化一次。

标量行为参考 [MySQL 标量子查询](https://dev.mysql.com/doc/refman/8.4/en/scalar-subqueries.html)
和[子查询错误](https://dev.mysql.com/doc/refman/8.4/en/subquery-errors.html)。
IN 参考 [MySQL 三值比较](https://dev.mysql.com/doc/refman/8.4/en/any-in-some-subqueries.html)
及[子查询限制](https://dev.mysql.com/doc/refman/8.4/en/subquery-restrictions.html)。
[依赖所有者设计](design.md#runtime-非相关标量依赖所有者) 说明边界、复杂度与迁移方式。

UPDATE/DELETE 现已共享查询依赖图：`WITH ... UPDATE/DELETE` 可在 WHERE、SET 和计算排序表达式中使用
非相关标量、IN/EXISTS 及命名 CTE；单表写入支持 MySQL 的可选 `AS`/裸目标别名，直接子查询也可用
该别名逐行引用当前候选。声明别名后原表名不再是表达式限定符；Catalog 查找和物理写入仍使用原表名。
读取和两遍候选物化完成后先关闭全部查询来源，再执行写入，
因此目标表 CTE 读取保持同一事务快照且写阶段不持有 relation lease。递归 CTE DML 与查询一致，
只有配置了正数 `sql_max_recursive_iterations` 才开放。

直接相关子查询也可由 `UNION [ALL]` 和查询分组组成；所有叶子与复合尾部共享 SQL 参数加外层行的
固定参数布局，每个外层行会完整重开并关闭该复合执行树。复合叶子可使用无 FROM、普通表、
仅含普通表的 JOIN，或归属于该直接子查询的嵌套派生/非递归 CTE 来源；定义查询也可为复合查询。

多级 scalar/IN/EXISTS 表达式查询链现可同时捕获父查询和祖先查询的当前行。名称从最近层向外解析；
内层声明的同名限定符即使缺少目标列，也遮蔽更外层限定符。无 FROM 的中间查询不引入可见列；
普通表、JOIN、派生表及非递归 CTE 可作为各层的输入，UNION 分支按各自 SELECT 输入绑定。
SQL 参数槽保持原始顺序，随后按最近层到最外层追加捕获行。构造期间只准备来源 schema，绑定结束
便释放临时来源租约；嵌套回调同步借用父层行，并在关闭执行器后清空借用槽，TEXT/BLOB 输出仍深拷贝。

分组后的 SELECT/HAVING/ORDER BY 子查询现可捕获当前分组的直接列键，包括重排、重复键、别名及
序号引用。最深层引用也参与祖先层分组校验；WHERE、GROUP BY 键求值及聚合参数内的子查询仍使用
分组前输入行。未分组的源列捕获返回 SQL_ERROR；沿用既有严格分组约束，不推断主键、唯一键或
WHERE 常量所带来的函数依赖，也不从复合分组表达式反推源列。

多级捕获也可跨派生表和非递归 CTE 定义边界。定义只继承所属查询可见的外层 frame；定义内部的
表达式子查询再加入定义自身的当前输入行。相关 CTE 的嵌套 reader 随求值关闭，先释放 reader 再
重建 store；非相关 CTE 保持原有语句缓存。协议见[跨定义捕获](design.md#跨定义捕获与-cte-reader-生命周期)。

设置正数 `sql_max_recursive_iterations` 时，递归 CTE 的 seed/member 可直接捕获外层列，或通过
前置相关 CTE 继承捕获。每个外层回调从新的 seed/cache/frontier 开始；原 SQL 参数仍使用语句
快照，非相关递归缓存保持复用。支持分组键、多个 reader 和当前已支持的 JOIN/UNION 形态。
执行重开协议见[相关递归 CTE](design.md#相关递归-cte-的执行重开协议)。

递归定义内部的 scalar/IN/EXISTS 子查询也可捕获 seed/member 当前行与祖先行，并可跨内部
派生表、非递归及递归 CTE 定义。先绑定 seed 依赖并发布 nullable schema，再绑定 member
依赖，在同一编译 owner 中完成构造。原 SQL 参数快照、严格分组、预算和失败原子性沿用原契约；
自引用表仍只允许出现在 member 的直接 FROM 中，不能移入子查询。
协议及验证范围见[递归内部捕获](design.md#递归定义内部的词法-frame-与分阶段绑定)。

显式 LATERAL 派生表已通过正常依赖图和 runtime 接入 关系执行器。前缀按实际
JOIN 准备顺序绑定，RIGHT JOIN 先准备右侧；完整前缀保持 SQL 列序和已完成的 NULL 扩展，
前缀间保持同一词法层级。来源只能引用已准备好的前缀和所属查询可见的祖先行；自身、
后续未准备来源及不合法的外连接方向在打开时拒绝。普通非 LATERAL 派生表仍不能引用
同一 SELECT 的兄弟来源，依据 [MySQL LATERAL 规则](https://dev.mysql.com/doc/refman/8.4/en/lateral-derived-tables.html)。

每个节点只编译一次并拥有原 SQL 参数快照，FROM 按当前左行重开整个相关右子树，普通
叶子 rewind，LATERAL 叶子调用成对 open/close；相关 CTE 每行重建缓存，非相关 CTE
保持语句缓存。嵌套 LATERAL 单独激活，scalar/IN/EXISTS 可同时使用局部行和祖先捕获。
配置正数递归上限后，LATERAL 内可执行递归 CTE，也可捕获递归 member 的直接 self
来源；self 表仍不能进入子查询。构造和 EXPLAIN 不读业务行或求值，空前缀不激活 child。

AST、输入参数及 marker 字节载荷可在成功打开后释放；捕获借用只存活到当前轮次成功关闭。
关闭失败保留借用与 owner，仅允许释放消费者后重试 close；首错锁定，重复读取不再执行
或扣额度。步骤超限后清理仍能运行。无磁盘格式、ORM ABI 或连接配置变化。
直接 `LATERAL (SELECT MAX(a.id))` 在 child 内聚合已验证，完整跨查询聚合重定位仍未实现。
普通私有 FROM 执行入口继续拒绝 LATERAL，runtime 明确选择相关调度。
见 [FROM 调度协议](design.md#from-相关子树调度协议) 与
[runtime 接入协议](design.md#lateral-依赖来源与-runtime-接入)。
相关 TEXT/BLOB 标量结果由依赖节点深拷贝并保留到消费者关闭，
每次实际求值占一个 MATERIALIZED_ROWS 单元并受 WORK/步骤额度约束。
当前不宣称完整 MySQL 兼容，M4/M5/M6 仍在实施。

无 FROM 查询支持 `SELECT 1`、`SELECT ?`、WHERE、既有聚合/分组、DISTINCT、排序和分页，
以恰好一行内部输入开始；过滤后可为空，全局 COUNT(*) 在空输入上仍返回 0。
内部占位列不参与 SQL 名称解析，列引用及星号必须有 FROM。没有显式别名时，输出标签取表达式
源码，最多 63 字节；更长表达式须写别名，不截断。标签不等于可引用的标识符，派生表/CTE
输出仍须满足既有名称规则，可用显式别名或 CTE 列名列表命名；重复输出名仍拒绝。
纯常量查询不读取 Catalog 或业务行，但仍占用事务来源租约至 close；EXPLAIN 的 table/type
为 NULL，Extra 含 `No tables used`，不求值投影。仍不提供虚拟 `FROM DUAL`、无分组无聚合的
HAVING 或 MySQL 的完整自动列名规则。语法依据 [MySQL SELECT](https://dev.mysql.com/doc/refman/8.4/en/select.html)
及 [EXPLAIN](https://dev.mysql.com/doc/refman/8.4/en/explain-output.html)，实现边界见
[无 FROM 查询设计](design.md#无-from-查询的单位行输入)。

事实｜无 FROM 增量新增 11 个正式用例，Windows Release 的 11 个相关 CTest 目标全部通过
（44.60 秒）：runtime 107 项/145428 条断言，真实 relational 插件 68 项/3094 条断言，
SELECT/FROM/JOIN/UNION/expr/scan/aggregate/subquery/CTE store 回归通过。
构建无警告或错误，空白检查通过；未运行 MySQL 服务端差分、sanitizer 或 benchmark。

CTE 接入已完成私有 [共享物化缓存](src/cte_store.h)：第一次读取完整物化，后续引用以独立游标读取
同一份不可变结果。缓存复用 Rows 的有界深拷贝和 Scan 类型校验；取消、失败和消费者租约均有
明确状态，部分结果不会在失败时公开。非递归 WITH 已在此基础上接入名称绑定和执行依赖图。
见[所有权与接入设计](design.md#cte-共享物化与独立读游标)。

私有 CTE store 已增加有界递归迭代：编译后的成员计划每轮只读取上一轮新增行，结果类型由 seed
确定且全部可空；ALL 保留重复，DISTINCT 对数值/BOOL/NULL 元组累计去重。复用同一份 Rows
和独立 readers，失败不公开部分结果，轮次/工作区/物化行/步骤超限均明确返回错误。
私有结构 Binder 已保留词法自引用并校验初始/递归块顺序、直接 FROM 引用次数、外连接位置、
聚合/窗口及 UNION/分页限制；普通 SELECT 与递归检查共享聚合名称分类。
原生关系来源和 CTE reader 已增加私有 rewind：保留事务/schema/租约，要求消费者关闭，
累计读取与执行额度，不清除已有错误。正式组合测试逐轮重开编译后的 JOIN，重读原生表及共享
CTE 缓存并得到 `1 → 3 → 5`；缓存只物化一次。见[来源重读协议](design.md#递归成员的来源重读协议)。

事实｜来源重读新增 11 项用例，Windows Release 的 8 个相关 CTest 目标全部通过（59.10 秒）。
relation 101 项/14052 条断言、CTE store 21 项/4447 条断言；递归核心、SELECT/FROM/JOIN、
runtime 和真实 relational 插件保持通过。构建无警告或错误，空白检查通过。
未运行 MySQL 服务端差分、sanitizer 或 benchmark。

```powershell
cmake --build --preset win-release-user --target orm_tidesdb_sql_relation_test orm_tidesdb_sql_cte_store_test
ctest --preset win-release-user -R "^orm_tidesdb_sql_(relation|cte_store)$" --output-on-failure
```

**MED｜当前阶段：** 关系驱动已通过 `sql_max_recursive_iterations` 显式准入有界递归；
部分成员分页及其他 M4/M5/M6 工作仍未完成。私有 compound 入口从同一 AST
分别构造 seed/成员计划，保留
seed 内部 UNION、分组及排序分页；参数沿用全语句位置，AST 释放后可复用成员计划迭代。
混合节点的全局尾部留给递归 owner，不在分部执行中应用。
见[CTE 分部计划构造](design.md#从同一-ast-构造-cte-的两个查询部分)。
私有递归查询 owner 已统一持有两部分计划、schema、参数快照、独立自引用来源及延迟缓存，
从递归 UNION 边推导累计去重。成员外部来源通过明确的轮次回调重开，表达式缓存保留；
关闭失败保留资源供重试，迭代上限必须显式提供。该 owner 已支持完整定义上的递归分页，
见[查询 owner 协议](design.md#递归-cte-的查询-owner)。依赖图已有独立私有递归构造入口，
验证直接自引用后连接该 owner，其余 CTE 引用仍做拓扑检查；成员派生来源按轮次重开，
seed 及 scalar/IN/EXISTS/CTE 缓存保持原状态。完整内部查询可在 AST 释放后执行递归定义链、
嵌套作用域、派生查询和原生 JOIN。未配置递归限额时仍走原有递归拒绝入口。
见[依赖图协议](design.md#递归定义接入依赖图)。
私有 `orm_sql_runtime_open_recursive` 已统一拥有整条语句、参数快照及递归依赖图，要求正数
迭代上限，支持执行重开和 default/TRADITIONAL EXPLAIN。EXPLAIN 为每个定义组合 seed/成员，
SELECT id 全语句连续；递归 SELECT 的首个物理表行标记 `Recursive`，JOIN 其他行不重复标记。
不执行递归、不建立业务缓存、不读取业务行；仍绑定并校验类型、参数和依赖。
保留项目自己的 QUERY GROUP/UNION RESULT 结构描述，不声称复现 MySQL 优化器输出。
见[递归 EXPLAIN 协议](design.md#递归-cte-的-explain-组合)。
全局 `LIMIT/OFFSET` 复用已有参数 Binder；缓存达到接受行数上限后停止取行和开启新轮次，
偏移前缀仍作为递归输入，外部引用只读取分页结果。`LIMIT 0` 不执行 seed/成员。
完整定义外层的括号分页可组合；覆盖部分成员的分页继续明确拒绝，不能误用为全局上限。
EXPLAIN 增加分页元数据，不执行分页或递归；排序/去重等上游阻塞算子仍可能先准备输入。
见[全局递归分页协议](design.md#递归-cte-的全局分页)。
事实｜分页新增 17 项测试；递归缓存累计 39 项/22652 条断言，CTE plan 累计 58 项/33980 条
断言通过。Windows Release 12 个相关目标全部通过（73.33 秒），构建无警告或错误，空白检查
通过。未运行 MySQL 服务端差分、sanitizer 或 benchmark。
事实｜本阶段新增 8 项测试，CTE plan 累计 49 项/29438 条断言通过；Windows Release 的
12 个相关 CTest 目标通过（51.83 秒），包括 runtime 和真实 relational 插件。最终构建无
警告或错误，空白检查通过；未运行 MySQL 服务端差分、sanitizer 或 benchmark。
事实｜依赖图新增 13 项用例，CTE 目标累计 41 项/21356 条断言通过；12 个相关 CTest 目标
通过（72.45 秒）。收尾恢复普通查询的临时表释放时机后，runtime/CTE/真实插件 3 个目标复验
通过（51.80 秒），构建无警告或错误，空白检查通过。未运行 MySQL 服务端差分、sanitizer 或 benchmark。
事实｜owner 新增 14 项用例后，CTE plan 共 28 项/12476 条断言通过；Windows Release 的
12 个相关 CTest 目标全部通过（86.15 秒），含参数载荷、外部派生/标量依赖、关闭重试和全部
构造/执行分配及步骤失败。构建无警告或错误，空白检查通过；未运行 MySQL 服务端差分、
sanitizer 或 benchmark。
共享 CTE schema Binder 已复制 seed 类型及名称、应用显式列名，并按递归规则标记所有列可空；
成员宽度/类型在执行前检查，不反向拓宽 seed。普通 CTE 已复用该入口，保持原可空性。
多个递归成员可使用独立 frontier reader，共享同一轮 Rows 范围，各自推进游标；不复制增量行，
也不读取本轮追加结果。遗留 reader 会阻止轮次/缓存释放，失败后可显式清理重试 close。
见 [seed schema](design.md#cte-的-seed-schema-所有权) 与[独立 frontier 游标](design.md#多成员的独立-frontier-游标)。

事实｜本阶段新增 14 项用例。Windows Release 的 11 个相关 CTest 目标全部通过（66.67 秒），
递归核心 31 项/22345 条断言、runtime 133 项/149489 条断言、真实插件 68 项/3094 条断言。
补充默认名称所有权及显式重命名用例后，CTE Binder 28 项/3663 条断言复验通过（0.03 秒）。
覆盖 seed/AST 释放、NULL/宽度/类型、多个成员固定轮次范围、累计去重、泄漏 reader 的关闭重试、
逐分配和全部步骤失败。构建无警告或错误，空白检查通过；未运行 MySQL 服务端差分、sanitizer
或 benchmark。

```powershell
cmake --build --preset win-release-user --target orm_tidesdb_sql_cte_bind_test orm_tidesdb_sql_cte_recursive_test
ctest --preset win-release-user -R "^orm_tidesdb_sql_cte_(bind|recursive)$" --output-on-failure
```

分部计划的正式集成用例位于
[cte_plan_test.c](tests/integration/cte_plan_test.c)，覆盖多 seed/成员、
累计去重、右嵌套边界、参数编号、seed 类型合并、原生表 JOIN 重开、无部分结果的执行错误、
元数据 EXPLAIN、深度上限，以及逐分配和全部构造步骤失败。
事实｜新增 14 项/4318 条断言通过；Windows Release 共 12 个相关 CTest 目标通过（66.18 秒），
含 runtime 133 项及真实 relational 插件 68 项。构建无警告或错误，空白检查通过；未运行
MySQL 服务端差分、sanitizer 或 benchmark。

```powershell
cmake --build --preset win-release-user --target orm_tidesdb_sql_cte_plan_test
ctest --preset win-release-user -R "^orm_tidesdb_sql_cte_plan$" --output-on-failure
```

私有 runtime 已支持分离关闭/重开 SELECT、JOIN、UNION 及括号树的执行状态，保留已绑定计划，
不重解析 SQL。带自有依赖图的 root 通过私有 resume 使用原始参数快照，按拓扑重开派生查询、
重置外层 CTE 引用游标；非相关 scalar/IN/EXISTS 和 CTE 缓存保留，不重开其内部生产者。
TEXT/BLOB 参数深拷贝并计入工作区，原始参数可在首次 open 后释放；内部 scope 查询仍由外部
所有者提供参数。见[参数与依赖重开协议](design.md#同一语句的参数与依赖重开)。

事实｜同一语句依赖重开新增 14 项用例。Windows Release 的 11 个相关 CTest 目标通过（62.86 秒）；
补充顶层 UNION/CTE 链测试后，runtime 133 项/149461 条断言复验通过（25.35 秒）。真实插件
68 项/3094 条断言回归通过。覆盖原始字节参数、深层派生来源、缓存边界、事务快照、逐分配及
每个重开步骤失败、已物化缓存的失败清理。构建无警告或错误，空白检查通过；未运行 MySQL
服务端差分、sanitizer 或 benchmark。复验沿用下方 runtime/relational 命令。

递归工厂可调用该入口，正式测试在 AST 释放后逐轮重开 JOIN 并得到 `1 → 2 → 3 → 4`。
详见[执行状态分离协议](design.md#编译查询的执行状态分离)。

事实｜执行重开新增 12 项 runtime 用例；Windows Release 的 11 个相关 CTest 目标全部通过
（62.09 秒）。runtime 119 项/147350 条断言、真实插件 68 项/3094 条断言；关系来源及各相邻算子
回归通过。包括每个固定分配及重开步骤失败、参数载荷所有权、外部表达式依赖租约、错误锁定和
runtime 驱动的递归 JOIN。构建无警告或错误，空白检查通过；未运行 MySQL 服务端差分、sanitizer
或 benchmark。

```powershell
cmake --build --preset win-release-user --target orm_tidesdb_sql_runtime_test orm_tidesdb_sql_relational_test
ctest --preset win-release-user -R "^orm_tidesdb_sql_(runtime|relational)$" --output-on-failure
```

DISTINCT 累计索引目前每轮重建，长递归链可能受到步骤预算限制，未作性能收益声明。
详见[递归轮次与缓存协议](design.md#递归-cte-的轮次与缓存协议)。

事实｜结构绑定新增 19 项测试/2394 条断言；Windows Release 的 13 个相关 CTest 目标全部通过
（38.64 秒）。覆盖作用域遮蔽、多个初始/递归成员、嵌套自引用拒绝、外连接两侧、分页树、
混合 UNION、逐分配/逐步骤故障。runtime 107 项/145428 条断言、真实插件 68 项/3094 条断言
保持通过。构建无警告或错误，空白检查通过；未运行 MySQL 服务端差分、sanitizer 或 benchmark。
详见[结构绑定协议](design.md#递归-cte-的结构绑定协议)。

```powershell
cmake --build --preset win-release-user --target orm_tidesdb_sql_cte_bind_test orm_tidesdb_sql_select_test
ctest --preset win-release-user -R "^orm_tidesdb_sql_(cte_bind|select)$" --output-on-failure
```

事实｜递归核心新增 26 项测试/9597 条断言；Windows Release 的 12 个相关 CTest 目标全部通过
（29.80 秒）。包括真实编译 SELECT 的多行递推/Fibonacci、NULL/重复/环、字节生命周期、
独立读游标、轮次/资源耗尽、回调重入、清理重试及逐分配/逐步骤失败。
既有 runtime 107 项/145428 条断言、真实插件 68 项/3094 条断言保持通过。
构建无警告或错误，空白检查通过；未运行 MySQL 服务端差分、sanitizer 或 benchmark。

```powershell
cmake --build --preset win-release-user --target orm_tidesdb_sql_cte_recursive_test orm_tidesdb_sql_cte_store_test
ctest --preset win-release-user -R "^orm_tidesdb_sql_cte_(recursive|store)$" --output-on-failure
```

非递归 WITH 支持多个定义、同层前序定义引用、嵌套作用域、外层可见名称、显式列名列表、
SELECT/UNION/分组/派生表以及 scalar/IN/EXISTS 内的 WITH。CTE 采用一次惰性物化，每个 FROM
出现位置持有独立读游标；CTE 名称优先于同名 Catalog 表。显式列名列表须唯一且匹配查询宽度，
未写列名列表时沿用既有计算列别名要求。未被求值的 CTE 不读业务行，但仍验证名称和类型。
EXPLAIN 为每个定义输出一次 DERIVED，不物化缓存。RECURSIVE 关键字可用于无自引用的 WITH；
实际递归引用须配置 `sql_max_recursive_iterations`。详见[绑定与依赖图](design.md#非递归-with-的绑定与依赖图)，语法依据
[MySQL WITH 文档](https://dev.mysql.com/doc/refman/8.4/en/with.html)。

事实｜WITH SQL 接入新增 12 个用例，Windows Release 的 11 个相关 CTest 目标全部通过
（38.65 秒）。runtime 98 项/135491 条断言、真实 relational 插件 66 项/3033 条断言；
CTE store 17 项/4290 条断言回归通过。包括逐分配/逐步骤故障、事务回滚和取消释放。
构建无警告或错误，空白检查通过；未运行 MySQL 服务端差分、sanitizer 或 benchmark。

```powershell
cmake --build --preset win-release-user --target orm_tidesdb_sql_cte_store_test orm_tidesdb_sql_subquery_test orm_tidesdb_sql_expr_test orm_tidesdb_sql_scan_test orm_tidesdb_sql_aggregate_test orm_tidesdb_sql_select_test orm_tidesdb_sql_from_test orm_tidesdb_sql_join_test orm_tidesdb_sql_union_test orm_tidesdb_sql_runtime_test orm_tidesdb_sql_relational_test
ctest --preset win-release-user -R "^orm_tidesdb_sql_(cte_store|subquery|expr|scan|aggregate|select|from|join|union|runtime|relational)$" --output-on-failure
```

事实｜缓存新增 17 个用例、4290 条断言通过；Windows Release 的 6 个相关 CTest 目标全部通过
（36.96 秒），覆盖缓存、scan/join/subquery/runtime/真实 relational 插件。构建无警告或错误，
空白检查通过。未运行 MySQL 服务端差分、sanitizer 或 benchmark。

```powershell
cmake --build --preset win-release-user --target orm_tidesdb_sql_cte_store_test orm_tidesdb_sql_subquery_test orm_tidesdb_sql_scan_test orm_tidesdb_sql_join_test orm_tidesdb_sql_runtime_test orm_tidesdb_sql_relational_test
ctest --preset win-release-user -R "^orm_tidesdb_sql_(cte_store|subquery|scan|join|runtime|relational)$" --output-on-failure
```

事实｜标量 raw SQL 接入新增 14 个用例；Windows Release 的 10 个相关 CTest 目标全部通过
（27.61 秒）。runtime 62 项/47220 条断言、真实 relational 插件 57 项/2620 条断言、
subquery 68 项/12578 条断言通过。覆盖嵌套/UNION/分组/JOIN、事务快照、AST/参数生命周期、
NULL/多行/短路、EXPLAIN、取消、深度、逐分配故障及每个构造步骤预算边界。
构建无警告或错误，git diff --check 和新增文件空白检查通过。
未运行 MySQL 服务端差分、sanitizer 或性能 benchmark。

复验：

```powershell
cmake --build --preset win-release-user --target orm_tidesdb_sql_subquery_test orm_tidesdb_sql_runtime_test orm_tidesdb_sql_relational_test
ctest --preset win-release-user -R "^orm_tidesdb_sql_(subquery|runtime|relational)$"
```

事实｜IN/NOT IN raw SQL 接入新增 12 个用例。最终 Windows Release 复验：subquery 70 项/12679
条断言、runtime 70 项/68713 条断言、真实 relational 插件 59 项/2708 条断言全部通过（33.60 秒）。
相邻 expr/scan/aggregate/select/from/join/union 七个 CTest 目标此前同轮回归通过。
覆盖有符号/无符号及 NULL 左值、字节参数所有权、缓存共享、嵌套/UNION/JOIN/分组、事务快照、
EXPLAIN、LIMIT 拒绝边界、延迟错误、物化上限、逐分配故障和每个构造步骤预算边界。
最终构建无警告或错误，git diff --check 及新增依赖文件空白检查通过。
未运行 MySQL 服务端差分、sanitizer 或性能 benchmark。

EXISTS 接入的内部准备已增加 `orm_sql_select_open_cardinality`：只观察结果行状态，
非 DISTINCT 查询跳过投影和排序、剪除 HAVING 不使用的聚合参数，保留分组、空输入全局聚合、
HAVING 和分页。DISTINCT 保留输出元组去重，避免 OFFSET 的结果行数被改变。
派生执行规格由 run 拥有，绑定计划可复用于普通 SELECT；省略的表达式不获取子查询来源租约。
该内部入口现由 raw EXISTS 的查询需求传播复用，普通 SELECT 仍使用原输出执行路径。
[设计及边界](design.md#exists-接入前的-select-行数观察路径)。

事实｜本阶段新增 11 个用例，SELECT 124 项/12975 条断言、subquery 73 项/12804 条断言通过。
Windows Release 的 10 个相关 CTest 目标全部通过（38.12 秒）；runtime 70 项/68713 条断言、
真实 relational 插件 59 项/2708 条断言通过。包含每个构造步骤及逐分配失败、查询来源租约、
忽略投影/排序/无用聚合的错误、保留 HAVING 错误、DISTINCT/OFFSET 和普通计划复用。
构建无警告或错误，git diff --check 通过；未运行 MySQL 服务端差分、sanitizer 或 benchmark。
复验：构建 `orm_tidesdb_sql_select_test orm_tidesdb_sql_subquery_test` 后执行
`ctest --preset win-release-user -R "^orm_tidesdb_sql_(select|subquery)$"`。

非相关派生表 `FROM (SELECT …) AS d` 已接入，可以嵌套并包含既有分组、UNION、排序和分页，
也可作为 INNER/LEFT/RIGHT/CROSS JOIN 的来源。派生来源与外层共享事务快照，打开时绑定类型，
按需取行，未强制物化全表；JOIN 等算子的物化继续受现有配额约束。EXPLAIN 输出 DERIVED，
不执行其投影或读取业务行。AST/参数载荷可在 open 后释放；关闭外层消费者后才释放子查询。
遵循 [MySQL 派生表规则](https://dev.mysql.com/doc/refman/8.4/en/derived-tables.html) 的别名和唯一列名要求；
支持别名后的等宽列清单 `AS d(x,y)`，按输出顺序覆盖名称，可重命名星号展开、UNION、
匿名计算列和重复的内部输出名称；普通及 LATERAL 派生表共用该规则。未提供清单时，
带 FROM 的计算列仍须显式别名。重复清单名称、列数不匹配或引用被覆盖的旧名返回
SQL_ERROR，绑定不修改子查询的类型、行值、GROUP/ORDER 名称和参数顺序。
名称由执行 owner 拥有，AST/参数可在 open 后释放；没有派生表合并或条件下推优化。
内部同名输出若被 ORDER/GROUP/HAVING 引用，必须对应同一表达式，否则报歧义。
事实｜列清单增量新增 19 个用例，21 个 parser 与 TidesDB SQL CTest 全部通过
（114.56 秒）；真实插件 150 项、CTE plan 168 项、FROM 66 项，包含构造分配和步骤
故障覆盖。[绑定协议及复验命令](design.md#派生表显式列名称协议)。
[所有权与失败设计](design.md#非相关派生表的-from-来源)。

事实｜派生表接入新增 12 个用例。Windows Release 的 10 个相关 CTest 目标全部通过（34.84 秒）；
补充来源租约保护后，subquery/runtime/真实 relational 插件 3 个目标复验通过（34.66 秒）。
最终 runtime 88 项/108473 条断言、真实插件 64 项/2942 条断言；FROM 26 项/2737 条断言。
覆盖 AST/字节参数生命周期、按需读取、空集 schema、嵌套混合依赖、UNION 分页、外连接、
EXPLAIN、事务回滚、取消、错误恢复、逐分配故障及每个构造步骤预算边界。
构建无警告或错误，空白检查通过。未运行 MySQL 服务端差分、sanitizer 或 benchmark。

EXISTS / NOT EXISTS 已接入 raw SQL，遵循 [MySQL 存在性规则](https://dev.mysql.com/doc/refman/8.4/en/exists-and-not-exists-subqueries.html)。
[查询需求与复合执行设计](design.md#exists-查询需求与复合执行) 说明何时省略投影、何时保留去重以维护分页行数。
名称、类型、UNION 宽度及参数仍在打开时校验；不存在相关列绑定或隐式类型转换。

事实｜本轮新增 12 个用例，Windows Release 的 10 个相关 CTest 目标全部通过（32.60 秒）：
runtime 79 项/90143 条断言、真实 relational 插件 62 项/2845 条断言，SELECT 124 项及 subquery 73 项回归通过。
覆盖多列/星号/NULL/NOT、惰性缓存、忽略无用投影/排序/聚合、分页去重、JOIN/HAVING、嵌套、参数、
事务回滚、EXPLAIN、取消、逐分配故障和每个构造步骤边界。构建无警告或错误，空白检查通过。
未运行 MySQL 服务端差分、sanitizer 或 benchmark；不据此宣称完整 MySQL 兼容。

```powershell
cmake --build --preset win-release-user --target orm_tidesdb_sql_subquery_test orm_tidesdb_sql_expr_test orm_tidesdb_sql_scan_test orm_tidesdb_sql_aggregate_test orm_tidesdb_sql_select_test orm_tidesdb_sql_from_test orm_tidesdb_sql_join_test orm_tidesdb_sql_union_test orm_tidesdb_sql_runtime_test orm_tidesdb_sql_relational_test
ctest --preset win-release-user -R "^orm_tidesdb_sql_(subquery|expr|scan|aggregate|select|from|join|union|runtime|relational)$" --output-on-failure
```
