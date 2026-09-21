# TurboDB 运行时驱动 Factory 设计规范

日期：2026-09-18
状态：正式规范草案，等待 committed-spec review；架构方向已批准，本文的详细 API/兼容性决策尚不代表已实现或已发布。
跟踪：[Epic #29](https://github.com/qigao/turbodb/issues/29)、[SDK/spec #30](https://github.com/qigao/turbodb/issues/30)；所有权前置：[#28](https://github.com/qigao/turbodb/issues/28)。
核对基线：`master@855e9c3eeb77d41fa1fc20fc44cc01ebe9c77a41`。

本文固定目标结构、接口语义、资源归属、失败行为与验收边界，不是 implementation plan。本次提交只增加本文件；不改变生产代码、公开头文件、测试、workflow、CMake 或数据库数据。#30 只有在规范审查及 SDK 实现/测试均完成后才能关闭。

## 1. 目标、非目标与决定

**固定每个 OS/arch/build configuration 下的应用和核心二进制，用户仅改变配置和部署文件，即可选择一个或多个数据库驱动。** 不再通过 `ORM_WITH_*` 重编核心。驱动仍由发行方独立编译、测试和发布，不是运行时编译，也不是要求所有平台都有全部驱动。

决定采用：数据库无关 core + 显式 runtime + module loader + driver registry/factory + 独立驱动模块。加载的是 TurboDB driver，不是将原生 `libmysql` 当作 ORM 插件。驱动内部正常链接 native client；核心不逐个查找 `mysql_*` 或 `PQ*` 符号。

| 方案 | 取舍 |
| --- | --- |
| 全驱动编入核心，factory 按名称选择 | 能运行时选择，但核心仍携带数据库实现/链接依赖，不满足目标 |
| 核心动态加载每种 native client | 可延迟 native 依赖，却仍需修改核心来增加数据库，不采用 |
| 独立 TurboDB 模块注册统一 factory | 驱动与核心分离，复用既有 backend 抽象；采用，并承担显式 ABI/所有权成本 |

非目标：热升级、单驱动热卸载、任意目录扫描、自动下载、恶意插件沙箱、驱动间服务发现、跨数据库事务、连接池重写、统一 migration 工具或新 `turbodb-mysql` CLI。不声称动态加载等于非阻塞、零拷贝或性能提升。

## 2. 已核对事实与影响

以下是基线事实，不是新能力：

| 位置 | 事实与本设计的影响 |
| --- | --- |
| [orm_internal.h](https://github.com/qigao/turbodb/blob/855e9c3eeb77d41fa1fc20fc44cc01ebe9c77a41/orm/src/abi/orm_internal.h) | 已有 backend factory/ops，但 plan 含 `vec_t`、`tstr` 私有布局；不能直接发布该头作为 SDK |
| [PostgreSQL component](https://github.com/qigao/turbodb/blob/855e9c3eeb77d41fa1fc20fc44cc01ebe9c77a41/orm/src/dbs/postgres/component.c) | 已通过 `orm_connect_with_factory_v1` 接入，可复用流程，但此 helper 仍是私有组件边界 |
| [ORM CMake](https://github.com/qigao/turbodb/blob/855e9c3eeb77d41fa1fc20fc44cc01ebe9c77a41/orm/CMakeLists.txt) | SQLite/Redis/MongoDB 按选项进入核心，TidesDB 始终进入核心；PostgreSQL 是独立链接组件而非插件 |
| [orm.h](https://github.com/qigao/turbodb/blob/855e9c3eeb77d41fa1fc20fc44cc01ebe9c77a41/orm/include/orm/orm.h) | C ABI 为 4；结果类型没有专门的 decimal/时间；公开接口已依赖 Salts/CBind/CFlow |
| [orm_core.c](https://github.com/qigao/turbodb/blob/855e9c3eeb77d41fa1fc20fc44cc01ebe9c77a41/orm/src/abi/orm_core.c) | disconnect 直接销毁 backend；延迟命令借用 query/connection，因此模块 pin 不能代替 #28 原生对象所有权 |
| [cursor contract](https://github.com/qigao/turbodb/blob/855e9c3eeb77d41fa1fc20fc44cc01ebe9c77a41/orm/src/flow/orm_cbind_publisher.h) | reader/token 借用持续至整个 Publisher resume 返回；WAIT 和 row_shape 有独立寿命，迁移必须保留 |
| [C++ wrapper](https://github.com/qigao/turbodb/blob/855e9c3eeb77d41fa1fc20fc44cc01ebe9c77a41/orm/include/orm/orm.hpp)、[C++ flow test](https://github.com/qigao/turbodb/blob/855e9c3eeb77d41fa1fc20fc44cc01ebe9c77a41/orm/tests/flow/orm_cpp_flow_test.cpp) | 包装只持有 C handles；构造入口需迁移，查询/Publisher 形态和正确的销毁顺序应保持 |

本次只能通过 GitHub 文件接口阅读；本地无 CodeGraph，容器 GitHub DNS 请求失败，因此没有完整本地 checkout、编译或生命周期运行证据。文档审查不能替代后续验证。

## 3. 强制不变量

| ID | 契约 |
| --- | --- |
| D01 | 最终 core 及其传递链接闭包不包含任何数据库 backend/native client，包括 TidesDB；通用 Salts 依赖可保留 |
| D02 | 应用只链接通用 `Orm::C`/`Orm::Cpp`；加载所选模块，不提前 import 每种 driver |
| D03 | runtime 持有自己的 registry；连接绑定自己的 factory 结果，无全局 `current_driver` |
| D04 | 同一 runtime 可用多驱动，同一驱动可创建不同参数的多个连接；事务只属于一个连接 |
| D05 | 插件只使用公开 SDK，不包含核心私有头，不读写 `vec_t`/私有字符串布局，不暴露 native handle |
| D06 | 只加载用户显式指定的可信模块；缺失、冲突、不兼容立即报错，不选择替代数据库 |
| D07 | 任何可执行插件代码的对象、任务、WAIT 回调和在途调用结束前，模块不得卸载 |
| D08 | #28 是 query/connection/transaction/Publisher 所有权的唯一实现归属；#31 组合该契约而非另建镜像 registry |
| D09 | SQL 方言在驱动，portable `?N` 输入契约保留；原生 SQL 不承诺跨库通用 |
| D10 | 结果/参数/控制面结构均有容量上限；取消不伪装成已终止，不重放可能已提交的写入 |
| D11 | 同一二进制与真实依赖隔离通过 hash、实际加载路径及真实服务验证，不以源码存在或 mock-only 代替 |
| D12 | 不新增 CMake install/verify 相关代码，不用安装探测、自动补库或 profile fallback 掩盖错误 |

## 4. 组件与依赖方向

| 组件 | 拥有 | 不拥有 |
| --- | --- | --- |
| Core facade | query plan、limits、通用 CSerde/CBind/CFlow 消费链、连接/事务外壳 | SQL 方言、数据库 SDK、native client |
| Runtime | 有界 registry、注册事务、模块句柄、控制面状态、诊断 | 用户数据库配置文件解析、业务事务 |
| Loader adapter | load/symbol/close 的平台语义及错误转换 | factory、数据库 ID 列表、数据库网络连接 |
| Driver SDK | 版本化 POD/opaque 契约、只读 plan accessor、ops 与 lifetime 约定 | 私有实现布局、跨工具链任意兼容承诺 |
| Driver module | native 库适配、方言、连接上下文、cursor/transaction 实现 | 全局应用状态、其他驱动的连接 |
| Application | 显式加载清单、数据源配置、凭据、执行器/owner loop 的有效寿命 | native ORM 所有权镜像 |

独立模块 target 使用 `turbodb_driver_sqlite/postgresql/mysql/redis/mongodb/tidesdb` 命名。名称是发行约定，不是 core 中的枚举。通用 SQL helper 可作为驱动侧共用代码，但不得通过核心的依赖闭包重新拉入具体数据库。

复用 Salts 容器、同步与 Executor。当前检索只确认若干直接加载的调用点，未确认满足受限搜索契约的公开 loader API；#31 开工时必须核对其实际 SDK。存在合适接口则薄适配，否则在单一 ORM platform adapter 实现最小 load/symbol/close。不得复制 benchmark 的不受限加载策略，或另建通用插件框架。

## 5. 公共 Runtime API

下面固定的是待实现接口声明与语义，不是当前可运行的示例。类型定义与实现由 #30 的后续 SDK 工作交付。

| 签名（均采用 `ORM_C_CALL`） | 参数、返回与寿命 |
| --- | --- |
| `void orm_runtime_config_init(orm_runtime_config_t *config)` | 初始化版本、预算和默认 fail-fast cleanup handler，不分配资源；调用者提供完整可写结构 |
| `orm_status_t orm_runtime_create(const orm_runtime_config_t *config, orm_runtime_t **out, orm_error_t *error)` | 校验版本与预算；成功给调用者一个强引用；失败 `*out=NULL`，不加载数据库 |
| `orm_status_t orm_runtime_load_driver(orm_runtime_t *runtime, const orm_driver_load_config_t *config, orm_error_t *error)` | 显式路径与期望 canonical ID；加载、校验、初始化、原子注册；失败不改变已有注册 |
| `orm_status_t orm_runtime_driver_info(orm_runtime_t *runtime, orm_string_view_t id, orm_driver_info_t *out, orm_error_t *error)` | 查询已注册能力与 ID；只返回有界、复制到调用者结构的元数据，不暴露模块句柄 |
| `orm_status_t orm_runtime_connect(orm_runtime_t *runtime, const orm_config_t *config, orm_connection_t **out, orm_error_t *error)` | 按已注册 ID/别名创建新连接；不隐式加载，成功转移独立 backend 所有权；失败 `*out=NULL` |
| `orm_status_t orm_runtime_close(orm_runtime_t *runtime, orm_error_t *error)` | 检查并关闭资源，不释放 handle 内存；有依赖或在途准入返回 BUSY 且不改变状态；已关闭返回 OK |
| `void orm_runtime_retain(orm_runtime_t *runtime)` / `void orm_runtime_release(orm_runtime_t *runtime)` | retain 要求已有有效强引用；release 消耗一个引用，不等价于已同步关闭；NULL release 无操作 |

connection/query/transaction 的 close/retain/release 在第 7 节固定；本表只列 runtime 控制入口。

新增配置均以 `uint32_t struct_size/abi_version` 开头；保留字段置零。path/ID/option 采用显式长度，禁止嵌入 NUL。调用者在函数返回前保持输入有效；需要延迟使用的数据必须在返回前有界复制，不保留 config/options 的裸借用。

`orm_runtime_config_t` 固定控制面预算：max_drivers、max_aliases_per_driver、max_connections、max_module_path_bytes、max_pending_operations、max_control_bytes；默认分别为 16、4、256、4096、256、1048576，均可显式调整为非零合法值。ID 固定上限 63 字节；bootstrap descriptor 上限 65536 字节；越界返回 LIMIT_EXCEEDED，而非扩容或截断 ID/path。

`orm_driver_load_config_t` 包含 size/version、module_path、expected_driver_id，首版 flags 必须为零。`orm_driver_info_t` 包含 size/version、canonical ID 固定容量 buffer、capabilities、execution-model bits、SDK bundle ID；所有输出 buffer 均由调用者提供，不返回借用 descriptor 指针。runtime config 还包含版本化 execution config 与第 7 节的 cleanup-error handler/context；handler/context 有效期覆盖 runtime 的全部延迟清理。

执行策略通过版本化 execution config 显式选择：`CALLER_BLOCKING` 或 `OWNER_EXECUTOR`。前者不创建线程；后者引用调用方提供的有界 Salts Executor/owner context，并要求它持续有效至所有完成与清理回调退出。缺失必需执行设施返回 INVALID_ARGUMENT，不退回阻塞执行。具体 Salts 适配不得依赖未经核对的私有接口。

控制面 load/connect/close 是同步接口，可以包含磁盘、认证或 native 清理等待，不允许宣称可直接在事件循环热路径非阻塞使用。`OWNER_EXECUTOR` 的同步控制调用不得从同一执行 owner 内等待自身任务；检测到该使用方式应拒绝，不造成死锁。

### 5.1 ID、别名与多 Runtime

canonical ID 必须是小写 ASCII `[a-z][a-z0-9_-]*`。别名由已验证 descriptor 声明；PostgreSQL 可声明 `postgres`，MongoDB 可声明 `mongo`，core 本身不内置这些映射。load 的 expected ID 必须等于 canonical ID；connect/info 可以使用已注册别名。

同一 runtime 中，重复 canonical ID、重复别名、同一规范化路径再次加载一律返回 DRIVER_ALREADY_REGISTERED，不覆盖、不按优先级挑选。所有别名随 canonical 项一次性登记；任一冲突回滚整次注册。同驱动的多个数据源应重复 connect，不应重复 load。

runtime 只拥有注册项，不把这些内部注册项计入阻止自身 close 的 dependent lease；外部 connection/operation 才沿 parent 链持有 runtime，避免 runtime 与 registry 形成引用环。即使 native 连接已 close，仍持有它的 owner handle 也应在 runtime 最终 close 前 release，使关闭顺序明确。

不同 runtime 的 registry、配置和连接互不共享。OS 可以共享模块映射；驱动中 process-wide native 初始化必须独立计数并同步，不能关闭另一个 runtime 或外部合法使用者的 native 库。无法满足共享初始化契约的组合不得列入支持矩阵。

## 6. Driver SDK 与 bootstrap ABI

新增公开 `orm_driver_abi.h`，与应用 facade 分层；不安装 `orm_internal.h`。模块仅导出固定入口 `orm_driver_get_api_v1`，其余实现符号默认隐藏。所有入口和回调使用统一 C calling convention；禁止跨边界抛出 C++ exception。

入口签名为规范声明：`int32_t ORM_DRIVER_CALL orm_driver_get_api_v1(const orm_driver_host_v1 *host, uint32_t host_bytes, const orm_driver_api_v1 **out_api, uint32_t *out_api_bytes)`。

bootstrap 只交换明确布局的整数、指针及长度；返回 status，不使用未经兼容性验证的 CFlow 对象。调用前两个 out 清零；失败保持零。成功返回模块只读静态 descriptor 及其真实可访问长度，寿命持续至卸载。

主 descriptor 字段按顺序包括：size/version header、32 字节 SDK bundle compatibility ID、canonical ID view、aliases pointer/count、capability bits、execution-model bits、module ops、create_connection factory、connection ops。host descriptor 包含同样的 header/compatibility ID，以及分组的公开 plan/lifetime/execution 服务表。表中每项均有完整 SDK 类型声明，不能用无类型可变参数协议替代。

兼容性 ID 来自发行 SDK 的固定 manifest，覆盖 driver ABI、公开 ORM/Salts/CFlow/CSerde/CMeta ABI 组合及目标平台/架构/CRT 配置。模块与 host 必须精确匹配该 ID；这个约束不是任意编译器二进制兼容的证明，也不验证 native client 的所有依赖冲突。

### 6.1 校验与原子发布顺序

先验证实际 buffer 长度足够读取固定 8 字节 header，再读取 version/size；要求 size 不超过实际长度且覆盖 v1 必需前缀。每张子表独立按相同规则验证；任何可选尾字段只有在 size 覆盖时才能读取。已知前缀之外的字节不解释，未知必需 capability/ABI 拒绝。

随后验证 compatibility ID、canonical ID、别名预算、必要回调和能力组合；host 与 driver 双向校验。bootstrap 不启动线程、不建立连接、不保留 host 指针；只有全部校验成功才执行 module initialize。模块构造器由 OS 执行这一事实无法由入口校验消除，故只接受可信模块。

host 在锁内预留注册/容量与 module pin，锁外调用 loader/插件，最后在锁内一次性发布 registry。插件初始化失败必须释放自身部分资源，host 回收 handle/预留项；已注册其他模块不变。初始化后如发布因冲突失败，用已验证 module finalize 清理；不得调用无效 descriptor 内猜测出的 destroy 指针。

### 6.2 小接口表与能力

| 表 | 职责与必需关系 |
| --- | --- |
| module ops | initialize/finalize；每 runtime 一个模块上下文；initialize 失败 out 为零 |
| connection ops | destroy、open_cursor、execute_command、begin_transaction；factory 在 descriptor 单独提供 create_connection；destroy 始终必需，其余受明确能力控制 |
| transaction ops | destroy、open_cursor、execute_command、commit、rollback、savepoint、rollback_to_savepoint、release_savepoint；savepoint 三项必须成组 |
| cursor ops | next、cancel、destroy，以及可选 configure_shape/column_count；next/cancel/destroy 必需 |
| plan metadata ops | describe、column_at、ordering；只读访问 kind/table/raw SQL/limit/offset/counts |
| plan value ops | assignment_at、predicate_at、raw_parameter_at；只读标量/bytes，不返回内部容器 |
| lifetime ops | acquire/release 受限 operation lease；只能对当前合法 parent 派生，不能复活已关闭 owner |
| execution ops | 有界提交、请求取消与完成通知；使用宿主 trampoline，并与 operation lease 配对 |

能力至少区分 query-plan kinds、raw SQL、事务、savepoint、隔离等级、增量结果、native WAIT、executor-offloaded execution。非 SQL 后端不得因存在函数指针就声明 SQL/隔离语义。unsupported 操作在 dispatch 前或驱动明确验证处返回 UNSUPPORTED，不调用空指针或伪造成功。

所有新增 SDK 结构使用 fixed-width size/version/tag/count、具名 reserved 字段和普通 C 布局；size_t 仅限经验证的本机 API 转换，不把其宽度当跨平台协议。view 长度为 uint64_t，转 native size 时先检查范围。严禁 bitfield、packed 私有对象或 native 数据库结构跨边界。

## 7. 唯一 Owner/Lease 契约（与 #28 对齐）

本规范选择 **原生 retained ownership + 显式 checked-close**，而不是依赖调用者自行维护镜像表。#28 交付 owner/lease 实现；#31 只把 module/runtime 的 parent lease 接上。以下新增所有权与 legacy 行为属于第 11 节的明确主版本迁移，不默默修改 ABI 4。

| 子对象/活动 | 必须持有的上级 |
| --- | --- |
| connection 与正在创建的 connection | module/runtime；factory 在调用前取得 admission lease |
| query | connection；plan 由 query 独占管理 |
| transaction | connection；其状态与查询的事务归属必须匹配 |
| 已打开但尚未消费的 Publisher | query；事务路径还持有 transaction |
| cursor、subscription、armed WAIT | execution owner；通过 owner 链保持 plan/connection/module |
| 排队/执行的 native task、完成通知 | operation owner；排队前取得 lease，取消不得提前释放 |
| 宿主进入插件的每一次调用 | 独立 in-flight module pin，持续至控制流实际返回宿主后 |

最后一行不可省略：插件回调中即使调用了 release，宿主也不能立刻卸载仍在执行该回调代码的模块。清理由宿主在回调返回后或 owner loop 的安全点执行；禁止模块内的最后一次 release 直接 `dlclose/FreeLibrary` 自己。

### 7.1 Checked-close、release 与 C++ 析构

`orm_connection_close(orm_connection_t *, orm_error_t *)`、`orm_query_close(orm_query_t *, orm_error_t *)`、`orm_transaction_close(orm_transaction_t *, orm_error_t *)` 与 runtime close 均返回 `orm_status_t`，关闭资源但不释放 handle 内存。有任何 dependent lease、在途准入、native call 时，返回 BUSY 且 owner 状态、资源和注册不变。关闭过的对象再次 close 返回 OK；新操作返回 INVALID_STATE。

connection/query/transaction 各提供 `void orm_<owner>_retain(orm_<owner>_t *)` 和 `void orm_<owner>_release(orm_<owner>_t *)`，与 runtime 规则一致。retain 不是对任意裸指针的安全探测：调用线程必须已持有有效引用。线程转交前取得引用；引用计数不得溢出，违反引用前置条件属于程序错误。release 仅消耗持有权；它不承诺同步 close 成功，不能被计为 checked-close 的成功证据。

活动 transaction 在没有 dependent lease 的情况下，checked-close 仍要求先显式 commit/rollback，否则返回 INVALID_STATE 且不改变事务。最后持有权的 release 可以触发未结束事务的 rollback 清理，但绝不 commit；失败走 cleanup-error 路径。COMMIT_UNKNOWN 事务保留未知结果，将连接标为不可继续业务，后续释放只安全清理本地/native session，不用自动 rollback 的返回值把原写入重新解释为已撤销。

最后一个外部引用释放但仍有子对象时进入 RELEASE_PENDING，父对象保留。已创建子对象可继续其合法操作并派生完成它们所需的 execution lease；不得从失效的外部 handle 新建兄弟对象。子对象和在途回调清空后，在所属 owner context 销毁 native 资源、再沿 parent 链释放。

新主版本的 `orm_disconnect`、`orm_query_destroy`、`orm_transaction_destroy` 保留 void 形态，明确作为消耗调用者引用的 legacy release 名称；正常逆序销毁仍立即清理，无依赖时行为不变。过早调用不再释放仍被借用的 native 状态。同步关闭结果由 checked-close 获取，不用 void API 隐藏 BUSY。

C++ 包装保持 move-only，只持有/释放原生引用；析构不得 throw，不创建第二套 ownership registry。显式 `.close()` 暴露状态，析构只 release。Publisher 的移动与析构必须转移/释放全部 native lease，而不是只搬运一个无 owner 保障的指针。

正常退出必须显式取消/销毁消费者，等待已提交 native work 完成，checked-close 依次成功，再 release 最后持有权。调用方必须保持执行器与 owner loop 可运行直到清理完成；本库不创建隐式后台线程来补救已停止的 executor。

### 7.2 并发与失败状态

控制面按 runtime 锁串行化 reservation/publication/close 准入。数据面每个连接最多一个 native operation 在执行；活动未消费结果或事务的更强 BUSY 限制由已声明 capability 决定。不同连接可以并行。CALLER_BLOCKING 连接由创建它的调用线程使用；OWNER_EXECUTOR 连接在声明的 native owner lane 执行。串行 lane 不自动等于固定 OS 线程，驱动声明的 thread-affinity 和 native thread-init 必须由适配层满足；执行器不具备所需约束时拒绝该模式，不假设线程安全。runtime 锁不得跨 native I/O、插件回调或等待执行器持有。

owner state、dependent count、in-flight admission 必须在同一个同步边界检查和更新。锁序为 runtime/module control → connection → transaction/query；可以拆阶段并持 lease，不能反向持锁调用 parent。最终 handle 释放前所有线程必须拥有各自引用；测试不得以访问已释放裸指针来要求未定义的并发安全。

close 的 BUSY 检查发生在任何销毁动作之前。通过检查才进入 CLOSING 并拒绝新准入，逆序 finalize/unload。若不可逆清理已经失败，不能报告 BUSY 并假装恢复 OPEN：进入 CLOSE_FAILED，返回明确错误，保留诊断与尚未确认可安全卸载的模块；只允许状态/诊断读取及释放应用引用，不自动重试未知清理或继续业务。

延迟 release 路径无法把清理错误返回原始调用者，因此 runtime config 必须提供 `on_cleanup_error` 回调；初始化函数默认安装 fail-fast handler，用户可显式替换。回调由宿主执行，不得重新进入该 runtime。失败对象被隔离，错误处理器不能把它恢复可用；残留隔离资源只在进程退出回收。这是报告过的故障路径，不是正常 pin 到进程退出的卸载策略。

## 8. Plan、Cursor 与资源预算

核心拥有唯一 query plan。插件只获取 opaque plan + 公开 accessor。读取返回 immutable view，禁止保存 vec/string 内部地址。创建 Publisher 时 acquire query execution lease；期间任何 plan mutation 返回 BUSY。未消费的 lazy command 也必须 pin plan，不能只保护已开始执行的 cursor。

参数与 options 在接口承诺期限内有界复制；驱动不将借用 config 跨 connect 返回保存。plan accessor 的 view 持续到对应 execution lease 结束且 plan 不再被执行引用。所有 bounds 检查在索引、加法、乘法及 native 长度转换之前。

cursor 继续使用公开 CSerde reader/CFlow waitable 的语义，不重新发明第二套流协议。这要求 SDK bundle ABI 匹配。next 仅在 ROW/ROW_AND_DONE 初始化 reader；其 context 与 token bytes 有效至整个 enclosing resume 完成同步 CBind 解码。不得 next 返回即释放行缓冲。

WAIT backing state 持续到 disarm/cancel 完成且潜在回调退出。cancel 是请求；只有确定不会再访问 context 后才能 destroy。row_shape 及其可达 metadata 由调用者保持至 Publisher/订阅完全清理结束；无法满足这一借用前提的调用方必须在上层拥有 metadata，而非让驱动猜测复制。

创建类接口均遵守：调用方 out 清零；成功交付完整且已验证的句柄；失败由创建方回收全部部分资源并保持 out 零。host 不能释放插件分配的 context；插件不能释放 host 分配的 plan。销毁操作只调用一次；由同一方分配/释放。首版不提供可混用的公共 allocator 注入：host 分配由 host 回收，driver 分配由 driver 的有效 destroy 回收，避免扩大 SDK。

owner handle、lease ticket、排队节点及注册元数据均计入 runtime 的 max_control_bytes，防止只限制连接数却无限创建 query/任务包装。预算覆盖注册项、路径/别名、连接数、排队 task、plan、参数、列数、每行缓冲和累计输出；满额返回 LIMIT_EXCEEDED/BUSY 的明确分类，不无界排队或静默丢弃。累计 max_result_rows/bytes 是输出限制，不等同于 native library 内部总内存硬上限；后者必须靠已验证的 client 配置和最坏场景测量另行声明。

## 9. MySQL 驱动契约

MySQL 是统一 SDK 的一个实现，不新增 core MySQL switch 或 `ORM_WITH_MYSQL`。native client 使用 libmysql，版本/平台由驱动发行 manifest 固定；不得未经验证替换为 MariaDB 并宣称相同 ABI。

SQL 方言由 driver 拥有，shared helper 不列举数据库名。MySQL 使用 prepared statements 和 positional `?`，禁止拼接转义后的参数值充当绑定。portable `?N` 必须同时重排绑定，例如 `a=?2 OR b=?1 OR c=?2` 转为三个 `?`，绑定次序为 2、1、2。同一 raw statement 混用 portable 与 native positional 标记时拒绝；重复/乱序允许，缺失/越界拒绝。

参数归一化必须识别字符串、quoted identifiers、注释及 MySQL SQL mode。首版对含可执行版本注释的 portable SQL 拒绝，不把其中 SQL 当普通注释忽略；native SQL 路径仍受原有安全限制。驱动生成标识符需验证并按方言引用，参数值不能用于标识符位置。

数据映射保持 NULL、signed/unsigned 64-bit、double、bool、TEXT、BLOB。嵌入 NUL、无符号上界、truncation、大字段均受检查。DECIMAL 默认保留精确十进制文本（含 scale），日期/时间保留服务器会话下的精确文本与小数秒；不转 double、不擅自推断时区。与 row_shape 要求不兼容时返回 TYPE_ERROR，不隐式丢失精度。

默认逐行未缓冲 fetch，不调用 `mysql_stmt_store_result` 全量缓存；native fetch 发现字段长度后，先检查预算再扩充应用缓冲。提前取消/读取失败后，必须安全 drain 或标记连接不可复用并在 native 工作退出后关闭，不把协议处于半读状态的连接交给下一个查询。

执行模型必须明确标注：MySQL prepared-statement 路径在首版按阻塞 native 调用处理。CALLER_BLOCKING 允许调用者明确承担阻塞；OWNER_EXECUTOR 使用有界任务、connection 串行 lane 与完成后唤醒。没有 executor 时拒绝要求非阻塞的配置，不在事件循环偷偷同步执行，也不把已有非阻塞 text-query API 的存在当成 prepared API 非阻塞证据。

事务覆盖 begin/commit/rollback/savepoint、affected_rows 和声明的隔离等级；不支持隔离立即拒绝。测试固定实际 storage engine；DDL/隐式提交不承诺可回滚。写入或 commit 响应丢失时保留 COMMIT_UNKNOWN，不自动重试/重放，不用简单连接错误推断服务端未执行。

native code/SQLSTATE 保留到有界诊断，凭据与参数值不进入应用日志/CI artifact。服务器自身日志策略由部署方配置，SDK 不声称能阻止服务器记录提交的 SQL。

## 10. 加载、错误与发行边界

加载策略、错误分类和发行闭包共同决定用户选择是否真实生效；三者必须作为同一部署契约验收，不用安装期探测弥补运行期歧义。

### 10.1 平台策略

模块路径必须是已规范化的可信绝对路径；注册前复制 ID/别名和所需诊断信息。禁止用户可写目录的自动搜索、环境变量隐式选驱动、URL 下载或 module-to-module 自动依赖发现。native 依赖仍由系统 loader 处理。

Windows adapter 使用宽字符受限加载，模块相邻依赖目录与必要系统目录显式限定；不调用进程全局 `SetDllDirectory` 来切换不同驱动。已加载同名依赖、系统重定向等仍可能影响绑定，必须记录实际路径，不能把受限搜索说成命名空间隔离。

POSIX adapter 使用立即符号解析与局部符号可见性；发行包用审核过的 RPATH/RUNPATH/依赖布局，测试清理会意外补库的环境。RTLD_LOCAL 不保证隔离已加载的同 SONAME/TLS 基础库。MySQL + PostgreSQL 的 TLS/压缩库必须交付兼容闭包并真实共存测试。

基线 [CompilerFlags.json](https://github.com/qigao/turbodb/blob/855e9c3eeb77d41fa1fc20fc44cc01ebe9c77a41/presets/CompilerFlags.json) 的 GNU shared flags 含 `-z,nodlopen`。#31/#35 检查实际 MODULE 及加载闭包 ELF 标记，定点移除冲突项，保留其他适用硬化；不能只改 EXE flags 后认为解决。

### 10.2 错误分类

新增状态在主版本 SDK 中明确分配，不复用已有数值改变其意义。业务 error 保持固定容量，driver error 另含 fixed-width native_code、6 字节 SQLSTATE buffer 和有界消息，host 在失效前复制；无全局 last_error。

| 条件 | 结果与副作用 |
| --- | --- |
| 未注册 ID | DRIVER_NOT_REGISTERED；不搜索磁盘 |
| 显式模块路径不存在 | MODULE_NOT_FOUND；不触发 connect |
| 路径存在但 OS load 失败 | MODULE_LOAD_FAILED；保留 OS code/阶段，涵盖依赖、格式、权限等 |
| 固定入口缺失 | DRIVER_ENTRY_MISSING；释放本次 handle |
| size/version/bundle ABI 错误 | ABI_MISMATCH；不调用未验证回调 |
| ID 不符或注册冲突 | DRIVER_ID_MISMATCH / DRIVER_ALREADY_REGISTERED；已有项不变 |
| 认证、网络、数据库选择失败 | CONNECTION_ERROR，阶段为 connect；不是 loader 错误 |
| 不支持、资源满、活动对象 | UNSUPPORTED / LIMIT_EXCEEDED / BUSY；不降级 |
| SQL、类型、约束、结果超限 | 原有对应状态并带安全诊断；释放本操作资源 |
| 已发写入/提交而结果不确定 | COMMIT_UNKNOWN；不自动重放 |
| 不可逆清理失败 | CLEANUP_FAILED + CLOSE_FAILED；隔离，不冒充 BUSY/OK |

不同 OS 未必能可靠区分“哪个传递依赖缺失”和其他 load 失败；不得根据模糊错误猜测。错误代码只承诺准确的已知阶段，详细 native diagnostic 保留其不确定性；测试用已知 fixture 证明依赖缺失路径。

### 10.3 包与支持矩阵

core/SDK 与每个 driver 分别有构建入口和依赖清单。数据库 `find_package` 与 vcpkg manifest 必须归 driver/tool，core-only configure 不拉取默认全数据库 SDK。发行方可选择构建 target；部署方无需重编 core/app。CMake MODULE target 不进入普通应用 link interface。

第一阶段验收目标为 Windows x64 与 Linux x86_64，SDK 同配置匹配；其余 OS/arch 仅记录 not validated/unsupported，不把 skip 当 pass。所有原生版本、compiler、CRT、Salts commit、数据库服务版本在对应实现 evidence manifest 中固定，本文不捏造尚未运行的已验证版本组合。

不新增 CMake install/verify 相关代码。依赖闭包检查、hash、部署清单、加载与真实服务测试放正常测试/CI 工具；已存在的安装验证机制不是本设计新增或扩展的复用点。#35 迁移该路径时应删除被替代的验证/兜底机制并保留直接 fail-fast 配置，不能用新安装探测补丁保留它。

## 11. 兼容性与主版本切换

公开 runtime 构造模型、所有权语义和数据库打包方式改变，必须采用显式主版本切换。本文选择目标 **ORM 3.0 / C facade ABI 5 / Driver ABI 1**；这是待审查的目标版本，不是当前已经发布的版本。

现有 ABI 4 / ORM 2.x 产物保持原身份，不用新 core 原地覆盖。过渡开发切片可存在旧 target，但不得将其称作最终通用 core；最终 3.0 发布 gate 前删除 builtin factory、core `ORM_WITH_*` 分支与 backend-specific core profiles。

3.0 不保留无 runtime 的 `orm_connect()` 和专属 `orm_postgresql_connect()` 作为新连接入口；编译迁移到 `orm_runtime_connect` 或 `runtime.connect(config)`。不修改旧函数签名后沿用同一 ABI，也不暗建默认 global runtime。保留的查询/事务/Publisher 消费方法沿用既有语义，void destroy 名称按第 7 节明确记录为 release；需要同步关闭状态的用户迁移到 close。

C++ 使用显式 runtime 创建 connection，既有 `query` builder 与 typed Publisher 转发尽量保持源兼容；配置和构造变化在迁移文档逐项列出。#28 服务的 TurboFlow 用户必须在 cutover 时明确选择旧版本或更新，不能混用 ABI 4 头文件和 ABI 5 二进制。

standalone SQLite/PostgreSQL schema tools 的 DDL、退出码、凭据输入与 native 依赖归各工具包，保持其现有语义；本 epic 不改为自动加载 ORM 插件，不新增统一 CLI，也不改变 Redis/TidesDB standalone 数据格式或 API 范围。

部署使用流程（协议示意，非当前可执行代码）：创建 runtime → 显式加载 mysql/postgresql 模块 → 创建 orders(MySQL)、archive(MySQL)、analytics(PostgreSQL) 三个连接 → 各自查询/事务 → 取消并销毁消费者 → 等 native work 完成 → close/release query/transaction/connection → close/release runtime。只用一种数据库时删去另一项 load 与部署文件，app/core 不变。

## 12. 交付顺序、验收与风险

| Issue | 范围与完成前提 |
| --- | --- |
| [#30](https://github.com/qigao/turbodb/issues/30) | 本规范 committed review 后，另写 SDK implementation plan；SDK 独立编译和 ABI contract tests 全部通过才完成 |
| [#28](https://github.com/qigao/turbodb/issues/28) | 原生 owner/lease、checked-close、legacy/C++ 生命周期，保留其原有下游验收，不因本 spec 关闭 |
| [#31](https://github.com/qigao/turbodb/issues/31) | 依赖 SDK 与 ownership；fake loader 可先验证，但不能冒充真实生命周期安全 |
| [#32](https://github.com/qigao/turbodb/issues/32) | SQLite/PostgreSQL 插件垂直切片，保留既有 SQL 行为；明确剩余 core DB 依赖尚未全部清理 |
| [#33](https://github.com/qigao/turbodb/issues/33) | MySQL 完整最小驱动及真实服务测试；不是仅 SELECT 1 |
| [#34](https://github.com/qigao/turbodb/issues/34) | Redis/MongoDB/TidesDB 抽离，与 MySQL 可并行；保留真实 capability |
| [#35](https://github.com/qigao/turbodb/issues/35) | 所有后端抽离后完成独立发行、legacy cutover 与旧选编删除 |
| [#36](https://github.com/qigao/turbodb/issues/36) | same-binary / 隔离 / 多驱动 / 生命周期最终证据；全部前置 gate 满足才关闭 epic |

### 12.1 必测证据

| 测试组 | 可复验标准 | 契约 |
| --- | --- | --- |
| SDK/negative ABI | 仅公开头的 C plugin/C++ consumer；短 descriptor、缺 callback、错误 ID/ABI/capability、失败 out 清零；不得越界读取 | D05/D06 |
| Core-only | 无数据库开发包 configure/build；实际 imports/DT_NEEDED 及传递闭包无 native DB；无 driver 仍能创建 runtime | D01/D02 |
| Same-binary | 每平台配置构建 app/core 一次；SQLite-only、PG-only、MySQL-only、MySQL+PG、两 MySQL 数据源场景前后 SHA-256 相同 | D03/D04/D11 |
| Missing dependency | 未选模块损坏/缺失不受影响；所选模块/依赖/入口缺失正确失败；不从系统 PATH 或旧 profile 偷补 | D06/D11 |
| Close/admission race | factory/任务预留与 close 竞争；BUSY 不变；在途 callback 最后一次 release 后返回前仍不卸载 | D07/D08 |
| Publisher/ownership | 未消费 lazy publisher、armed WAIT、订阅、事务、worker；正常/取消/bind失败/错误恰好释放一次，ASan/等效检查 | D07/D08 |
| Cleanup failure | 同步/延迟清理失败均明确报告，不能回到 OPEN 或重试不可重入 finalize；多 runtime 不提前终结共享 native 库 | D07/D10 |
| SQL/MySQL | 参数重复/乱序、注释/SQL mode、NULL/uint64/blob/decimal/时间、预算、affected_rows、rollback/savepoint、断连不确定写入 | D09/D10 |
| Runtime model | 阻塞模式不伪装 WAIT；有界 executor 满额拒绝，取消后 native 未退出仍 BUSY；owner-loop 停止前可证明排空 | D07/D10 |
| Regression/migration | 保留既有 SQLite/PG/Redis/Mongo/Tides tests、C++ 消费形态及 standalone 范围；major profile/ABI 不混用 | D01/D04/D12 |

每份实现证据包含 exact commit、run/job ID、命令、OS/arch/config、依赖/服务版本、文件 hash、实际部署清单、实际加载路径、测试数与 skipped 状态。fake tests 与 live tests 分列；passed/failed/unsupported/not run 不混淆。本次文档提交没有运行这些实现测试。

### 12.2 风险排序与审查重点

**HIGH — 所有权/卸载：** 早于 native completion 或 plugin return 的卸载会导致悬空回调。以统一 lease、宿主 trampoline、安全清理点和并发测试约束，不凭单后端行为推断全局安全。

**HIGH — 公开兼容：** runtime 构造和 retained release 改变 ABI/行为。以明确 3.0/ABI 5 cutover、旧产物不覆盖和下游迁移测试约束；本规范批准前不改变公开头。

**HIGH — 写入结果：** 断连/DDL/错误归类不当会导致重复写入或错误回滚承诺。保留数据库真实语义和 COMMIT_UNKNOWN，不自动重试。

**HIGH — Native 依赖冲突：** 局部符号和不同目录并不保证 TLS/同名库隔离。使用兼容闭包、实际加载检查及多驱动真实测试；不能承诺任意 client 版本混装。

**MED — 性能与复杂度：** registry 查询只在 connect/info，使用有界容器；查询热路径仅保持固定 ops dispatch，不做名称查找或 load。已批准 plugin 架构允许必要的 vtable 调用，不据一般热路径禁令消除接口边界。性能收益留给测量，不能预先声明。

**MED — 分阶段假完成：** SQLite/PG slice 仍可能有其余数据库核心依赖；#32 的通过不等于 D01/#35/#36 已完成，全部完成框须等对应证据。

## 13. 审查与后续边界

本规范审查覆盖：所有目标与 issue 对齐、必需字段先 size 校验、close/release 语义不冲突、所有借用有终点、不可逆失败不伪装 BUSY、MySQL 方言/执行模型及核心隔离有明确验收。源码阅读不等于运行验证，也不替代独立 reviewer。

提交后停在 committed-spec review。用户确认这份具体规范后，才编写 #30 SDK implementation plan；#28 的具体 ownership 实施同时对齐本契约。后续开发必须先建立 focused RED 证据再实现 GREEN，不提交空 API 或以禁用测试获得成功。

## 14. 官方依据

以下资料只支持所述基础 API 行为，不代表 TurboDB 新功能已验证：

- [CMake add_library / MODULE](https://cmake.org/cmake/help/latest/command/add_library.html)：运行时模块与普通链接库的区别。
- [Microsoft LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)：绝对路径与受限依赖搜索 flags。
- [GNU ld Options](https://sourceware.org/binutils/docs/ld/Options.html)：`-z nodlopen` 与实际对象加载限制。
- [MySQL prepared statement usage](https://dev.mysql.com/doc/c-api/8.4/en/c-api-prepared-statement-interface-usage.html)：原生绑定与参数标记。
- [MySQL mysql_stmt_fetch](https://dev.mysql.com/doc/c-api/8.4/en/mysql-stmt-fetch.html)：逐行未缓冲读取、全量 store-result 与 truncation。
- [MySQL asynchronous interface](https://dev.mysql.com/doc/c-api/8.4/en/c-api-asynchronous-interface.html)：非阻塞接口与同连接操作的边界；不据此推断 prepared 路径非阻塞。
