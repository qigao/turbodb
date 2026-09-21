# TurboDB Runtime Driver SDK Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. 没有可用 subagent 时采用逐任务内联执行，不声称存在独立 reviewer。

**Goal:** 为 #30 交付可单独编译的版本化 C Driver SDK、无数据库依赖的契约校验器、只读计划适配与可复验的 ABI/所有权契约测试，不提前实现 Runtime Loader 或 MySQL。

**Architecture:** 插件仅依赖公开 SDK 和声明匹配的 Salts 公共类型，核心私有 plan 通过只读适配器进入 SDK。先交付没有 OS 加载或数据库副作用的候选 SDK；真实 owner 接合必须使用 #28 的唯一原生实现，不在测试通过后默认认为它已经存在。现有 ABI 4 facade 不在这些基础任务中改义，最终 ORM 3.0 / C ABI 5 切换由 #35 收口。

**Tech Stack:** C11、C++17、TinyTest、CMake/CTest presets、Salts 公共 CMeta/CSerde/CBind/CFlow 类型；本计划不新增第三方库。

**Spec:** [运行时驱动规范](../specs/2026-09-18-runtime-driver-factory-design.md)，批准继续的固定版本为 [`2ab6a3e2d3f2809881d3b49caccf9d4fd5d5ee15`](https://github.com/qigao/turbodb/commit/2ab6a3e2d3f2809881d3b49caccf9d4fd5d5ee15)。其 Git blob 为 `b27e69d5061eed3859606edcb4b93c103cc7df2a`。

## Global Constraints

- “固定每个 OS/arch/build configuration 下的应用和核心二进制，用户仅改变配置和部署文件，即可选择一个或多个数据库驱动。” SDK-only 测试不等于该最终验收。
- 目标版本为 **ORM 3.0 / C facade ABI 5 / Driver ABI 1**；不是当前已发布版本，不把 ABI 4 二进制重新标成 ABI 5。
- “不新增 CMake install/verify 相关代码”，不增加安装期探测、补库、兜底或数据库 fallback。
- “#28 是 query/connection/transaction/Publisher 所有权的唯一实现归属；#31 组合该契约而非另建镜像 registry”。
- “任何可执行插件代码的对象、任务、WAIT 回调和在途调用结束前，模块不得卸载”。本计划没有 OS loader，不能仅凭它关闭该 gate。
- Runtime 预算默认值固定为 max_drivers=16、max_aliases_per_driver=4、max_connections=256、max_module_path_bytes=4096、max_pending_operations=256、max_control_bytes=1048576；这些属于后续 Runtime 配置，SDK 校验不另造一套默认值。
- ID 固定上限 63 字节；bootstrap descriptor 上限 65536 字节；SDK bundle ID 为 32 字节。
- canonical ID 为小写 ASCII `[a-z][a-z0-9_-]*`，别名不与 canonical ID 或同 descriptor 其他别名重复。
- core 不读取 native 数据库结构；插件不包含 `orm_internal.h`，也不依赖私有 `vec_t`、`tstr` 布局。
- 保留 portable `?N`、NULL、uint64、TEXT/BLOB 字节长度与精确类型；本计划不实现 MySQL 的 `?N` 方言重排。
- close 返回 BUSY 时不改 owner 状态；release 不代表同步 close；取消不等于 native work 已结束。
- 首期运行平台验收目标为 Windows x64 与 Linux x86_64；未运行或不支持项不记为通过。

---

## 0. 状态、工作范围与先后关系

用户在已提交规范后要求继续，本计划据此进入实施规划；不代表 GitHub 上已有独立审查批准、CI 通过或自动合并授权。本次只提交此计划，不执行下面的代码任务，所有完成框保持未勾选。

读取基线：PR #37 为 `design/runtime-driver-factory` → `master`，head `2ab6a3e2d3f2809881d3b49caccf9d4fd5d5ee15`；#28、#30 仍开放。执行者重新读取 PR/issue/head，若有他人改动先比较差异，不强推、不覆盖。

| 阶段 | 任务 | 可完成的声明 | 不能完成的声明 |
| --- | --- | --- | --- |
| A：纯 SDK | 1、2、3、4 | ABI/descriptor/回调契约、独立编译及失败回收可验证 | 真实驱动加载、核心依赖完全移除、原生所有权安全 |
| B：核心适配 | 5 | 私有计划只读映射与现有 SQL 回归可验证 | 无 lease 的延迟 plan 借用安全 |
| C：依赖接合 | 6、7，要求 #28 证据 | SDK 借用期限与真实 owner 保证一致 | OS module pin/卸载、Runtime、MySQL、多数据库共存 |

规范 §5 的 Runtime API 签名在 #31 实现，C++ Runtime 构造与发行切换在 #35；#30 此处提供其 driver contract，而不是发布没有实现的 runtime prototypes。规范 §9 的 MySQL 属于 #33；§10 的加载器属于 #31，发行属于 #35；same-binary/live-service 最终验收属于 #36。这样拆分不是删除要求，而是遵守既有 issue 归属。

### 文件归属

| 文件（新增，另有说明除外） | 单一职责 |
| --- | --- |
| `orm/include/orm/orm_driver_base.h` | calling convention、版本、固定宽度 view/header、SDK 专用 POD |
| `orm/include/orm/orm_driver_plan.h` | 只读 plan metadata/value DTO 与访问函数表 |
| `orm/include/orm/orm_driver_ops.h` | module/connection/transaction/cursor 与 host services 的类型化回调 |
| `orm/include/orm/orm_driver_abi.h` | bootstrap descriptor、固定入口声明；聚合上述三个公开头 |
| `orm/src/driver/orm_driver_contract.h/.c` | 无副作用的 prefix/view/descriptor/capability 校验；内部实现接口 |
| `orm/src/driver/orm_driver_plan_view.h/.c` | 唯一可见私有 plan 的 SDK 只读适配器；不加入插件 include 路径 |
| `orm/src/driver/orm_driver_owner_bridge.h/.c` | #28 到公开 SDK lifetime 表的薄适配，无第二套 owner 计数 |
| `orm/tests/driver/orm_driver_prefix_test.c` | 截断、版本、实际字节长度、view/整数边界 |
| `orm/tests/driver/orm_driver_descriptor_test.c` | bundle、ID、别名、子表、capability 一致性 |
| `orm/tests/driver/orm_driver_fixture.h/.c` | 仅测试的有状态 fake driver、计数与失败注入 |
| `orm/tests/driver/orm_driver_handshake_test.c` | 直接调用固定入口的握手、创建/失败/销毁协议 |
| `orm/tests/driver/orm_driver_plan_view_test.c` | plan DTO 值/边界/只读契约 |
| `orm/tests/driver/orm_driver_owner_bridge_test.c` | #28 完成后的真实 query/Publisher/transaction lease 接合 |
| `orm/tests/driver/consumer.c`、`consumer.cpp` | 仅公开 SDK 的 C/C++ 编译与链接 smoke consumer |
| `orm/driver-sdk/CMakeLists.txt`、`CMakePresets.json` | SDK-only 构建与测试入口，不进入顶层数据库 manifest，不含 install/export/verify 逻辑 |
| 修改 `orm/CMakeLists.txt` | 只添加 core-bound plan/owner 测试 target；不扩展旧安装验证机制 |
| `docs/architecture/runtime-driver-sdk.md` | 已实现接口、borrowed/owned 期限、测试命令与明确未实现范围 |

SDK 头可引用现有公开 `orm.h`/CFlow/CSerde/CMeta 类型，但不依赖它们的私有表示；bundle 必须描述实际头/运行库组合。前六个基础切片的候选 SDK 只在开发/测试 target 使用，不添加到 ABI 4 稳定安装头列表，不把候选描述符与旧 core 混合运行。未来可对公共基础类型做独立整理，但本计划不顺带重构整套 ORM facade。

### 构建入口与 RED 判据

Task 1 创建独立的 `orm/driver-sdk` source root，使用 `Salts CONFIG REQUIRED` 且唯一 prefix 为 `SALTS_ROOT`，调用者提供实际已安装的匹配 Salts。CMakePresets 使用 schema 6，故工具至少 CMake 3.25；这是既有 preset schema 的要求，不要求升级现有工程代码最低版本。新 preset 名为 `sdk-linux-debug`、`sdk-linux-release`、`sdk-windows-debug`、`sdk-windows-release`，各有同名 configure/build/test preset，binaryDir 为 `${sourceDir}/../../build/<preset-name>`；Ninja、Debug/Release、平台 condition 明确，继承环境中的编译器与 `SALTS_ROOT`，不写个人机器目录。

从仓库根进入 SDK source root 后统一使用同名 preset；每次执行前确认 SALTS_ROOT，而非复制一个推测值：

```sh
cd orm/driver-sdk
cmake --preset sdk-linux-debug
cmake --build --preset sdk-linux-debug --target orm_driver_prefix_test
ctest --preset sdk-linux-debug -R '^orm_driver_prefix$' --output-on-failure --no-tests=error
```

上述 binaryDir 展开为仓库根的 `build/sdk-linux-debug`；所有 SDK 任务使用同一组 preset，不猜测工作目录或从旧 build tree 补产物。SDK CMake 将 `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 与 `CMAKE_LIBRARY_OUTPUT_DIRECTORY` 都设为 `${CMAKE_BINARY_DIR}/bin`，静态产物为 `${CMAKE_BINARY_DIR}/lib`。

基础 SDK 测试只能依赖 Salts 通用 targets/SDK 与 TinyTest，不用 `orm_link_internal_test()`：该既有 helper 可能带入核心 backend。core-bound Task 5/6 使用既有 `linux-dev-user`/`win-dev-user` 与 `orm_link_internal_test()`，明确不把它们叫作 SDK/core 无数据库依赖证明。

RED 要保留失败命令、exit code、首个失败断言和 exact head。新增文件第一次缺头/缺符号可以作为编译 RED，但在生产实现前要补齐 fixture/setup，让行为断言在目标未实现时确实失败。找不到 Salts、测试未注册、编译器路径错误都只是环境错误；不得记为正确 RED。GREEN 要 `--no-tests=error`、精确测试清单/数量，不能以零测试退出成功收口。

## Task 1: 固定前缀、view 与数值边界

**Files:** 创建 `orm_driver_base.h`、`orm_driver_contract.h/.c`、`orm_driver_prefix_test.c` 和 SDK-only CMake/presets；将构建设置与这一可测试交付放在同一任务，不单独提交空脚手架。

**Interfaces:** 使用已有 `orm_status_t`（固定 int32）与既有状态值；新增以下完整基础接口。它们是实现后由 SDK/runtime 共用的内部校验器，不是已发布 facade API。

```c
/* orm_driver_base.h：使用 include guard、stdint.h 与 extern "C"；
 * ORM_DRIVER_CALL 在 Windows 为 __cdecl，其他平台为空。 */
typedef struct orm_driver_header_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
} orm_driver_header_v1;
typedef struct orm_driver_bytes_v1 {
  const void *data;
  uint64_t size;
} orm_driver_bytes_v1;
typedef struct orm_driver_table_v1 {
  const void *data;
  uint32_t bytes;
  uint32_t reserved;
} orm_driver_table_v1;

/* orm_driver_contract.h，返回现有 ORM_STATUS_*；无日志、无分配。 */
orm_status_t orm_driver_check_prefix(
    const void *buffer, uint32_t buffer_bytes, uint32_t expected_version,
    uint32_t required_bytes, uint32_t *out_struct_bytes);
orm_status_t orm_driver_check_bytes(orm_driver_bytes_v1 value,
                                     uint64_t max_bytes);
```

header 的两个字段 offset 固定 0/4、sizeof 固定 8；其他结构不采用 pack。`buffer_bytes` 是调用者保证可读的真实跨度，descriptor 自报 size 不是这个保证。不可信指针不是 ABI 校验能安全探测的对象；只测试真实分配的短 buffer。

- [ ] **RED：先写真实短 buffer 的行为测试。** 以下用 TinyTest 注册，并补正好 8 字节 header、required_bytes 超过自身 size、size 超过实际 buffer、未知 version、可忽略尾字段、0/65536/65537、空 out、NULL/nonzero bytes、uint64 到 SIZE_MAX 的边界。

```c
#include "orm_driver_contract.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

spec("driver prefix bounds") {
  it("never reads a physically shorter-than-header allocation") {
    for (uint32_t n = 0; n < 8; ++n) {
      unsigned char *p = malloc(n == 0 ? 1u : n);
      uint32_t out = UINT32_MAX;
      check_not_null(p);
      memset(p, 0, n == 0 ? 1u : n);
      check_equal(orm_driver_check_prefix(p, n, 1u, 8u, &out),
                  ORM_STATUS_ABI_MISMATCH);
      check_equal(out, 0u);
      free(p);
    }
  }
}
```

- [ ] **运行 RED：** 在 `orm/driver-sdk` 中执行 `cmake --preset sdk-linux-debug`、`cmake --build --preset sdk-linux-debug --target orm_driver_prefix_test`、`ctest --preset sdk-linux-debug -R '^orm_driver_prefix$' --output-on-failure --no-tests=error`。Windows 使用对应 sdk-windows-debug，期望失败集中在新契约而非 SDK 查找。
- [ ] **GREEN：** 先初始化 out，再检查可读 header 长度，用 memcpy 读取固定前缀，不先 cast 后解引用，不读取可选尾字段。最小分支顺序如下，常量在 base.h 命名为 `ORM_DRIVER_HEADER_BYTES`/`ORM_DRIVER_DESCRIPTOR_MAX_BYTES`。

```c
if (out_struct_bytes == NULL) return ORM_STATUS_INVALID_ARGUMENT;
*out_struct_bytes = 0u;
if (buffer == NULL) return ORM_STATUS_INVALID_ARGUMENT;
if (required_bytes < ORM_DRIVER_HEADER_BYTES)
  return ORM_STATUS_INVALID_ARGUMENT;
if (buffer_bytes < ORM_DRIVER_HEADER_BYTES)
  return ORM_STATUS_ABI_MISMATCH;
orm_driver_header_v1 header;
memcpy(&header, buffer, sizeof(header));
if (header.abi_version != expected_version ||
    header.struct_size < required_bytes || header.struct_size > buffer_bytes)
  return ORM_STATUS_ABI_MISMATCH;
if (header.struct_size > ORM_DRIVER_DESCRIPTOR_MAX_BYTES)
  return ORM_STATUS_LIMIT_EXCEEDED;
*out_struct_bytes = header.struct_size;
return ORM_STATUS_OK;
```

`check_bytes`：NULL/非零长度返回 INVALID_ARGUMENT；size 超过 max_bytes 或 SIZE_MAX 返回 LIMIT_EXCEEDED；其他返回 OK，允许合法空 view。不触碰 data 指向内容。

- [ ] **验证：** 运行同一 focused 测试、C/C++ sizeof/offsetof 检查与 Debug sanitizer；测试报告记录 sanitizer 是否实际启用，不能仅看 preset 名。
- [ ] **Commit：** `git add orm/include/orm/orm_driver_base.h orm/src/driver/orm_driver_contract.h orm/src/driver/orm_driver_contract.c orm/tests/driver/orm_driver_prefix_test.c orm/driver-sdk`；`git commit -m "feat(orm): add bounded driver ABI prefix contracts (#30)"`。

## Task 2: 类型化 Driver Ops、主描述符与双向校验

**Files:** 创建 `orm_driver_plan.h`（仅 DTO/表声明）、`orm_driver_ops.h`、`orm_driver_abi.h`、`orm_driver_descriptor_test.c`；扩展 contract.c 与 SDK test targets。

**Interfaces:** descriptor 使用规范规定的顺序；所有新 DTO 以 `orm_driver_header_v1` 开头，reserved 显式为零，所有 callback 均带 ORM_DRIVER_CALL。callback 中的 `orm_config_t`、`orm_error_t`、`orm_isolation_t`、`cserde_reader`、`cflow_waitable`、`cmeta_data_desc` 只从已有公开头取得，不复制它们的私有定义。

基础类型定义：`orm_driver_limits_v1` 在 header 后依次存八个 uint64：max_parameters、max_columns、max_predicates、max_assignments、max_query_bytes、max_parameter_bytes、max_result_rows、max_result_bytes。`orm_driver_value_v1` 为 header、uint32 kind/reserved 和具名 union data {int64_t sint; uint64_t uint; double real; uint8_t boolean; orm_driver_bytes_v1 bytes;}，kind 与现有 ORM_VALUE_* 做显式映射，未知 kind 拒绝。

plan DTO：`orm_driver_plan_meta_v1` = header、uint32 kind/flags、bytes table/raw_sql、uint64 column_count/assignment_count/predicate_count/raw_parameter_count/limit/offset。flags 的低三位依次为 select_all/has_limit/has_offset；未知位拒绝。`orm_driver_assignment_v1` = header、bytes column、value value；`orm_driver_predicate_v1` = header、bytes column、uint32 comparison/reserved、value value；`orm_driver_ordering_v1` = header、bytes column、uint32 present/order。

`orm_driver_plan_view_v1` 为 header、`const void *context`、metadata/value 两个 `orm_driver_table_v1`。context 仅由 host 解释。`orm_driver_plan_metadata_ops_v1` 表包含三个回调：`describe(context, meta*, error*)`、`column_at(context, uint64_t index, bytes*, error*)`、`ordering(context, ordering*, error*)`；`orm_driver_plan_value_ops_v1` 表包含 `assignment_at`、`predicate_at`、`raw_parameter_at`，后三者都是 `(const void *context, uint64_t index, <对应 DTO> *out, orm_error_t *error)` 返回 orm_status_t。索引统一零基；portable ?N 的一基标号转换属于 SQL driver。

每个 ops 表以 header 开头。下表的 `ctx` 类型都是 `void *`，`error` 都是 `orm_error_t *`，未注明的返回均为 `orm_status_t`；按表展开为具名 typedef 和 struct 字段，不用 varargs 或未类型化函数指针。

| 表 | 回调的完整参数与返回约定 |
| --- | --- |
| `orm_driver_module_ops_v1` | initialize(const orm_driver_host_v1 *host, void **out_context, error)；finalize(ctx, error) |
| `orm_driver_connection_ops_v1` | void destroy(ctx)；open_cursor(ctx, const orm_driver_plan_view_v1 *plan, const orm_driver_limits_v1 *limits, orm_driver_cursor_v1 *out, error)；execute_command(ctx, const plan*, const limits*, uint64_t *affected_rows, error)；begin_transaction(ctx, orm_isolation_t isolation, orm_driver_transaction_v1 *out, error) |
| `orm_driver_transaction_ops_v1` | void destroy(ctx)；open_cursor/execute_command 与 connection 表相同；commit(ctx,error)；rollback(ctx,error)；savepoint/rollback_to_savepoint/release_savepoint(ctx, orm_driver_bytes_v1 name, error) |
| `orm_driver_cursor_ops_v1` | next(ctx,cserde_reader *out_row,orm_driver_step_v1 *out_step,error)；void cancel(ctx)；void destroy(ctx)；configure_shape(ctx,const cmeta_data_desc *shape,error)；column_count(ctx,uint64_t *out_count,error) |
| `orm_driver_lifetime_ops_v1` | acquire(void *parent,void **out_lease,error)；void release(void *lease) |
| `orm_driver_execution_ops_v1` | submit(void *executor,void *owner,orm_driver_work_fn work,orm_driver_complete_fn complete,void *task_context,void **out_task,error)；void request_cancel(void *task)；void release_task(void *task) |

补充类型：`orm_driver_work_fn` 为 `orm_status_t (ORM_DRIVER_CALL *)(void *task_context)`；`orm_driver_complete_fn` 为 `void (ORM_DRIVER_CALL *)(void *task_context, orm_status_t result)`；完成通知必须由宿主 trampoline 调用，任务/lease 释放早于 work/complete 回调返回是协议错误。executor 表的真实实现由 #31 提供，本任务只验证表形状与能力组合。

`orm_driver_cursor_v1`/`orm_driver_transaction_v1`/`orm_driver_connection_v1` 均为 header、void *context、table ops。`orm_driver_step_v1` 为 header、uint32 kind/reserved、cflow_waitable waitable；kind 固定 ROW=0、ROW_AND_DONE=1、WAIT=2、DONE=3、ERROR=4。next 返回错误 status 时不交付 reader；WAIT 只允许声明 WAIT 能力的驱动。

```c
typedef struct orm_driver_host_v1 orm_driver_host_v1;
typedef struct orm_driver_api_v1 orm_driver_api_v1;
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_create_fn)(
    void *module_context, const orm_config_t *config,
    const orm_driver_limits_v1 *limits, orm_driver_connection_v1 *out,
    orm_error_t *error);
struct orm_driver_api_v1 {
  orm_driver_header_v1 header;
  uint8_t bundle_id[32];
  orm_driver_bytes_v1 canonical_id;
  const orm_driver_bytes_v1 *aliases;
  uint32_t alias_count;
  uint32_t reserved;
  uint64_t capabilities;
  uint64_t execution_models;
  orm_driver_table_v1 module_ops;
  orm_driver_create_fn create_connection;
  orm_driver_table_v1 connection_ops;
};
struct orm_driver_host_v1 {
  orm_driver_header_v1 header;
  uint8_t bundle_id[32];
  orm_driver_table_v1 plan_metadata;
  orm_driver_table_v1 plan_values;
  orm_driver_table_v1 lifetime;
  orm_driver_table_v1 execution;
};
```

capabilities 位 0–4 分别对应 SELECT/INSERT/UPDATE/DELETE/RAW_SQL；位 5–7 为 TRANSACTION/SAVEPOINT/INCREMENTAL_ROWS；位 8–12 对应现有五种隔离等级；其余置零。execution_models 位 0/1/2 分别为 CALLER_BLOCKING/OWNER_EXECUTOR/NATIVE_WAIT；NATIVE_WAIT 只描述数据面，控制面同步不因此变成非阻塞。整数常量均使用 UINT64_C(1) 左移并具名。

SDK 校验入口（在 `orm_driver_abi.h` 声明，Task 2 完整实现并以 SDK 静态辅助库提供）：`orm_driver_validate_host_v1(const void *host,uint32_t bytes,const uint8_t expected_bundle[32],orm_error_t *error)`；`orm_driver_validate_api_v1(const void *api,uint32_t bytes,const uint8_t expected_bundle[32],orm_driver_bytes_v1 expected_id,uint32_t max_aliases,orm_error_t *error)`。失败不执行任何描述符 callback；不持有指针；不分配 registry。#31 在其独立 ID 比较阶段返回 DRIVER_ID_MISMATCH，不把所有 INVALID_ARGUMENT 或消息字符串一概映射成 ID 错误。成功只证明当前可读不可变描述符满足声明契约，注册和模块 pin 属于 #31。

- [ ] **RED：** 写以下用例并逐一标注预期。fixture 每个字段先给合法值，再独立破坏一处，记录 init/factory/destroy 计数始终为零。

```text
buffer 0..7 字节                           -> ABI_MISMATCH
实际 buffer 小于主前缀/任一子表必要前缀      -> ABI_MISMATCH
错误 version/bundle                       -> ABI_MISMATCH
ID 空/大写/嵌入 NUL/首字符数字              -> INVALID_ARGUMENT
ID 长度 64 / aliases 超预算                -> LIMIT_EXCEEDED
期望 canonical ID 不同                     -> INVALID_ARGUMENT（SDK 验证层）
别名等于 canonical 或重复                  -> INVALID_ARGUMENT
必需 module init/finalize/create/destroy 空 -> ABI_MISMATCH
声明 SELECT/RAW 而无 open_cursor            -> ABI_MISMATCH
声明写命令而无 execute_command             -> ABI_MISMATCH
声明事务/隔离而无 begin_transaction         -> ABI_MISMATCH
SAVEPOINT 没有 TRANSACTION                 -> ABI_MISMATCH
未知能力/执行模式位                        -> UNSUPPORTED
完整合法描述符/合法额外尾字节               -> OK
```

- [ ] **运行 RED：** SDK source root 内 configure/build 同名 preset；target `orm_driver_descriptor_test`，CTest 名 `orm_driver_descriptor`，必须看到上述新断言失败。
- [ ] **GREEN：** 先调用 Task 1 prefix checker；主字段按已验证跨度 memcpy 到本地零初始化 POD。每个子表先检查 table.reserved 和实际 bytes，再检查自己的 header，最后按必要 offset 读取 callback。不要对“较旧但合法短尾表”直接复制 sizeof(full table)。ID 逐字节 ASCII 校验，不依赖 locale。别名比较最多 max_aliases^2，受调用方预算及控制字节预算约束。
- [ ] **验证：** 补 transaction 返回表的 commit/rollback 必需、savepoint 三项成组、cursor next/cancel/destroy 必需的检查函数，统一 `orm_driver_validate_{connection,transaction,cursor}_v1(const void *object,uint32_t bytes,uint64_t capabilities,orm_error_t *error)`。该组不调用 destroy 来清理 ABI 不可信对象；可信 driver 创建失败必须自清理，host 不能猜未知函数表。
- [ ] **Commit：** 仅添加本任务头、contract 修改及 descriptor 测试；提交 `feat(orm): validate typed driver descriptors and capabilities (#30)`。不增生产 runtime 符号。

## Task 3: 固定入口握手与独立有状态 Fixture

**Files:** 创建 `orm_driver_fixture.h/.c`、`orm_driver_handshake_test.c`，扩展独立 SDK CMake。

**Interfaces:** 唯一模块 export 如下；fixture 是 test-only 实现，不发布“空驱动”。

```c
ORM_DRIVER_EXPORT int32_t ORM_DRIVER_CALL orm_driver_get_api_v1(
    const orm_driver_host_v1 *host, uint32_t host_bytes,
    const orm_driver_api_v1 **out_api, uint32_t *out_api_bytes);
```

Windows 的 ORM_DRIVER_EXPORT 只在 module 编译时 dllexport；其他平台只对该入口 visibility(default)，C++ 用 extern "C"。bootstrap 双 out 有效才可写；任一 out=NULL 返回 INVALID_ARGUMENT，另一个若有效先归零；否则先归零双方、校验 host/bundle、成功交付 static const descriptor。bootstrap 期间不保存 host 指针；module initialize 在握手后才复制所需表并取得相应上下文。

fixture.c 仅包含公开 SDK 头；bootstrap 调用已经实现的公开 SDK 校验入口，不包含 `orm_driver_contract.h` 或任何核心内部头。fixture header 定义 `orm_driver_fixture_stats`（init_calls、finalize_calls、create_calls、destroy_calls、live_connections、next_calls、cancel_calls、cursor_destroy_calls，均 uint32），以及 `void orm_driver_fixture_reset(void)`、`orm_driver_fixture_stats orm_driver_fixture_stats_get(void)`、`const uint8_t *orm_driver_fixture_bundle(void)`（指向32字节静态测试 ID）、`orm_driver_host_v1 orm_driver_fixture_host(void)` 四个 helper。失败注入入口为 `void orm_driver_fixture_fail_next(uint32_t point)`，point 用 enum 明确 MODULE_BEFORE_ALLOC=1、MODULE_AFTER_ALLOC=2、CONNECTION_BEFORE_ALLOC=3、CONNECTION_AFTER_ALLOC=4，0 为不注入。固定 ID `contract_fixture`；只有 CALLER_BLOCKING/SELECT/INCREMENTAL_ROWS，其他能力不声明。module initialize 分配独立 context；create 分配独立 connection 并复制有界的测试选项 `fixture_value`；open_cursor 从连接值生成一行；next 第一次生成该单列 CSerde reader，第二次 DONE；cancel 标记终止；destroy 释放一次并递减计数；finalize 有 live connection 返回 BUSY，否则清理。reader 使用现有 `orm_cbind_publisher_test.c` 的公开 CSerde test reader 模式，不从私有 cursor 头拷贝结构。

失败注入是 fixture 自己的测试函数设置的一次性标志：module 分配前/后失败、connection 分配前/后失败。成功交付 context 后，清理责任转移；失败则本层立即清理且 out 全零。helper 与计数不 export 到模块。测试 host 的 lifetime 表用有界、可计数的 test-only ticket fixture 验证回调形状；它不是 #28 的原生 owner 实现。该 CALLER_BLOCKING fixture 不提交 executor 工作：host execution 表允许 data=NULL/bytes=0 表示不提供执行器；部分非空表必须完整校验。#31 在请求 OWNER_EXECUTOR 连接时对缺失表明确拒绝，不由本期 bootstrap 猜测未来连接策略。

- [ ] **RED：** 编写直接调用入口测试；无需 loader，尚不能声称动态加载成立。证明同一 host 的两次 create 产生独立 context，修改一个连接的 fixture_value 不改变另一个。

```c
const orm_driver_api_v1 *api = NULL;
uint32_t api_bytes = 0;
orm_driver_host_v1 host = orm_driver_fixture_host();
host.bundle_id[0] ^= 1u;
check_equal(orm_driver_get_api_v1(&host, sizeof(host), &api, &api_bytes),
            ORM_STATUS_ABI_MISMATCH);
check_equal(api == NULL, 1);
check_equal(api_bytes, 0u);
check_equal(orm_driver_fixture_stats_get().init_calls, 0u);
```

- [ ] **运行 RED：** target `orm_driver_handshake_test`，CTest `orm_driver_handshake`；预期是 bundle/out/副作用/失败回收断言失败，不能只是未链接 fixture。
- [ ] **GREEN：** 实现真实 fixture 生命周期与 bootstrap。契约校验器不负责调用 initialize，测试在双方校验成功后显式调用，再用 fixture ops 做创建/游标/释放。
- [ ] **验证：** 正常、取消、每个部分失败注入后 live_connections=0；两个连接各自产生自己的行；统计不把 bootstrap 当 initialize。asan 下无泄漏/双释放。线程/异步/原生 DB 不在该 fixture 的能力声明中。
- [ ] **Commit：** `test(orm): exercise driver handshake and failure ownership (#30)`。

## Task 4: SDK-only C/C++ consumer 与 MODULE 编译边界

**Files:** 创建 `consumer.c`、`consumer.cpp`，完善 SDK CMake/presets 与 descriptor/handshake target。公共头仅在本候选 SDK build interface 提供，不加入 ABI 4 稳定 package。

**Interfaces:** 独立 source root 提供 `orm_driver_sdk` INTERFACE（公共 include 与必要 Salts 类型）、`orm_driver_contract` STATIC（校验实现）、`orm_driver_contract_fixture` MODULE；C/C++ consumer 分别作为 executable，链接单独以测试模式编译的 fixture.c 以解析实际入口引用，不能链接 MODULE 本身。为直接入口测试单独编译同一 fixture.c 到 test executable，不能把 MODULE 当可链接 target。

CMake 的最小 target 关系：

```cmake
add_library(orm_driver_sdk INTERFACE)
target_include_directories(orm_driver_sdk INTERFACE ../include/orm)
target_link_libraries(orm_driver_sdk INTERFACE
  Salts::Core Salts::CSerde Salts::CBind Salts::CFlow)
add_library(orm_driver_contract STATIC ../src/driver/orm_driver_contract.c)
target_link_libraries(orm_driver_contract PUBLIC orm_driver_sdk)
target_include_directories(orm_driver_contract PRIVATE ../src/driver)
add_library(orm_driver_contract_fixture MODULE ../tests/driver/orm_driver_fixture.c)
target_link_libraries(orm_driver_contract_fixture PRIVATE orm_driver_contract)
set_target_properties(orm_driver_contract PROPERTIES
  POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
set_target_properties(orm_driver_contract_fixture PROPERTIES C_VISIBILITY_PRESET hidden)
```

prefix/descriptor 测试 harness 的内部 include 仅对各自 target PRIVATE 添加；不得经 SDK INTERFACE 传播。测试帮助头可从 tests/driver 找到，但 fixture/consumer 不允许访问 src/abi、src/flow、数据库 include。fixture 的公开 CSerde 实现按实际所用功能链接 Salts::CSerde；不是连接 turbo_orm 或任何数据库。consumer 若调用内部校验器，应只由测试 harness TU 调用；公共 consumer TU 本身只能 include SDK 头。

- [ ] **RED：** C consumer include `<orm_driver_abi.h>` 并检查 exported entry typedef 签名；C++ consumer 用 std::is_standard_layout 检查 POD、用 std::is_same 检查 calling convention 声明。缺 public include/意外 C++ mangling 应失败。

```cpp
#include <orm_driver_abi.h>
#include <type_traits>
using entry_fn = int32_t (ORM_DRIVER_CALL *)(const orm_driver_host_v1 *,
    uint32_t, const orm_driver_api_v1 **, uint32_t *);
static_assert(std::is_same<decltype(&orm_driver_get_api_v1), entry_fn>::value,
              "bootstrap calling convention changed");
static_assert(std::is_standard_layout<orm_driver_api_v1>::value,
              "driver descriptor must be a C-layout object");
int main() {
  entry_fn volatile entry = &orm_driver_get_api_v1;
  return entry != nullptr && sizeof(orm_driver_header_v1) == 8u ? 0 : 1;
}
```

- [ ] **运行 RED：** 单独 source root 配置，targets `orm_driver_c_consumer`、`orm_driver_cpp_consumer`、`orm_driver_contract_fixture`；禁止通过增加私有 include 路径修正失败。
- [ ] **GREEN：** 只修公共头依赖、C/C++ 守卫和 target 关系。debug/release 分树；Windows 不假定 Linux 编译足以证明 __cdecl/export；不继承旧 GNU 全局 nodlopen 设置作为 fixture 成功依据。
- [ ] **验证：** 运行 CTest `orm_driver_c_consumer`、`orm_driver_cpp_consumer` 及 Task 1–3。通过 `readelf --dyn-syms`/`dumpbin /exports` 检查 MODULE 仅公开约定入口（允许工具链必需符号），保留完整输出；不调用 dlopen/LoadLibrary。C/C++ smoke 与直接 handshake 分列，不能把 smoke main 的 0 当创建连接证据。
- [ ] **Commit：** `test(orm): verify public driver SDK consumer boundary (#30)`。

bundle 测试使用固定非生产 32 字节 ID，单字节不匹配即拒绝。不能从“相同测试 ID”宣称任意 Salts/toolchain 兼容。#35 的真实 bundle manifest 必须固定 SDK header hashes、C facade ABI、Salts/CFlow/CSerde/CMeta 版本、平台/架构/CRT；只读这些字段生成 ID，不能用时间戳/工作目录/数据库驱动名，才能让同 host 选择不同 driver。

## Task 5: 私有 Query Plan 到公开只读访问表

**Files:** 创建 `orm_driver_plan_view.h/.c`、`orm_driver_plan_view_test.c`；修改 `orm/CMakeLists.txt` 只添加 core-bound test target；复用现有 `orm_plan.c` 和 TinyTest 支持，不改 SQL renderer。

**Consumes:** Task 2 定义的 plan metadata/value DTO 与函数表；`orm_query_plan`/`orm_limits` 仅在内部 adapter/test 端使用。

**Produces:** 内部签名 `orm_status_t orm_driver_plan_borrow(const orm_query_plan *plan, orm_driver_plan_view_v1 *out, orm_error_t *error)`。它不取得新所有权，只把调用方已经保证存活且冻结的 plan 包装成只读 view；成功条件必须在文档中说明。公开插件不看到该函数/私有类型。真实异步调用方必须先通过 Task 6/#28 取得 execution lease，因此 Task 5 不直接改现有公开查询入口使用这条路径。

- [ ] **RED：** 按已核对 `orm_sql_render_test.c` 的 plan fixture 风格，用 orm_plan_init/add_bind 建 raw plan：SQL 为 `select ?2, ?1, ?2`，参数一是 3 字节 BLOB `{'a',0,'b'}`，参数二是 UINT64_MAX；分别读取第1、0、1号原始参数，要求值相同且 plan 未修改。补 NULL、空 TEXT、空 BLOB、structured assignments/predicates、limit/offset presence、超界 index 与无效 out。

```c
orm_driver_plan_view_v1 view = {0};
orm_driver_value_v1 first = {0};
orm_error_t error;
orm_error_init(&error);
check_equal(orm_driver_plan_borrow(&plan, &view, &error), ORM_STATUS_OK);
/* values 是从已验证 view.values.data 得到的 orm_driver_plan_value_ops_v1。 */
const orm_driver_plan_value_ops_v1 *values = view.values.data;
first.header.struct_size = sizeof(first);
first.header.abi_version = 1u;
check_equal(values->raw_parameter_at(view.context, 1u, &first, &error), ORM_STATUS_OK);
check_equal(first.data.uint, UINT64_MAX);
check_equal(values->raw_parameter_at(view.context, UINT64_MAX, &first, &error),
            ORM_STATUS_OUT_OF_RANGE);
```

DTO 中 union 字段名为 data，Task 2 各字段名称以本计划为准；初始版 header/size 全部核对。访问失败先使 out 值部分清零（保留已验证调用方 header），不返回旧值；失败 error 初始化与现有规则一致。

- [ ] **运行 RED：** 仓库根使用 `cmake --preset linux-dev-user` 与 `cmake --build --preset linux-dev-user --target orm_driver_plan_view_test orm_sql_render_test`；`ctest --preset linux-dev-user -R '^(orm_driver_plan_view|orm_sql_render)$' --output-on-failure --no-tests=error`。Windows 使用 win-dev-user。上述 core-bound 依赖缺失是环境 blocker，不把关闭后端变成新的测试方案。
- [ ] **GREEN：** context 转换仅在内部 adapter 中进行；describe 拷贝元数据，columns/assignments/predicates 用 `vec_at_const`，bytes 用公开 `tstr_len` 获取显式长度，不用 strlen。先比较 index 与 vec_size，再转 size_t；返回 view 不分配、不修改 plan。枚举逐项映射，未知 kind/order/comparison 返回 INVALID_ARGUMENT。
- [ ] **验证：** 私有 plan 的每一个字段都有对应 DTO 或明确表示；同 view 重复读取结果相同；SQL 字符串/标记仍原样，归一化留给 #32/#33。已有 orm_sql_render/public flow 测试不得回归。此任务不让插件读取 vec/tstr 内存布局。
- [ ] **Commit：** `feat(orm): expose read-only driver plan views (#30)`。

## Task 6: 与 #28 的原生 Owner 接合（硬前置）

**Files:** 创建 `orm_driver_owner_bridge.h/.c`、`orm_driver_owner_bridge_test.c`；修改 core-bound test target 与必要的已实施 #28 接入点。不得从本计划复制实现一套 query/connection 计数。

**前置证据：** #28 的真实代码必须已提供规范 §7 中的 checked-close/retain/release、query execution freeze、Publisher/subscription/transaction owner lease 与在途 native work 持有。只读 issue 为 open 不说明代码有无；执行时读取其实现 PR、exact head 和 focused/ASan 输出。缺少证据时保留本任务未执行状态，可先完成 Tasks 1–5；不能因此假实现公开接口或关闭 #30。

**Consumes/Produces:** SDK 的 `orm_driver_lifetime_ops_v1.acquire(parent,out_lease,error)`/`release(lease)` 接到 #28 的唯一内部 operation-lease 原语。bridge 对外提供内部 `const orm_driver_lifetime_ops_v1 *orm_driver_owner_services_v1(void)`；parent 必须是 host 当次交付的 #28 owner token，而非任意插件自造 connection 指针。不同实现 PR 可能使用不同私有命名，适配限于这个文件；不依靠尚未存在的 guessed retain 符号冒充 execution lease（普通 retain 不能阻止 plan mutation）。

- [ ] **RED：** 在真实 query 上打开未消费 Publisher；提前 query_close/connection_close 必须 BUSY 且 state 不变；参数 mutation 也 BUSY。销毁 Publisher/订阅并确认 native work 完成后，关闭 query 成功，再 release query，连接才能 close 成功。用 SQLite 与 #28 已验证的另一个有活动 cursor 保护的 backend 各执行一次。

```c
/* connection/query/publisher 是该测试按现有 public-flow fixture 创建的真实对象。 */
check_equal(orm_query_close(query, &error), ORM_STATUS_BUSY);
check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
cflow_publisher_cancel(&publisher);
/* 尚有未退出 worker 的 case 在释放 worker 完成闸门前仍必须 BUSY。 */
cflow_publisher_destroy(&publisher);
check_equal(orm_query_close(query, &error), ORM_STATUS_OK);
orm_query_release(query);
check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
orm_connection_release(connection);
```

单线程无 worker 的上述用例可以同步清理；worker case 用 condition/barrier 精确控制完成点，不使用 sleep 猜时序，不从已释放裸指针调用 close。cancel 请求后等待完成闸门前断言 BUSY，然后由宿主 completion 返回后释放 lease。

- [ ] **运行 RED：** core-bound target `orm_driver_owner_bridge_test`，CTest `orm_driver_owner_bridge`；保留对应 #28 head 与本任务 head。若已由 #28 满足所有行为，桥接服务自身的合法 parent/错误 parent/释放次数/冻结状态还须有会因缺少 bridge 而失败的测试，不能伪造生产回归。
- [ ] **GREEN：** acquire 在 #28 的同一个 admission 同步边界取得 operation lease；release 只归还该 lease，绝不直接销毁 parent 或卸载模块。host callback pin 留给 #31，bridge 不引入 OS 句柄。失败不交付 ticket；正确票据仅释放一次。
- [ ] **验证：** 正常终止、bind 失败、terminal error、取消、未消费 lazy command、事务活动 cursor、嵌套依赖和 release-before-child 完整覆盖；C++ wrapper 继续依赖同一原生 owner。CLOSE_FAILED 的处理按 #28 实现证据，不通过吞错得到 GREEN。
- [ ] **Commit：** `feat(orm): bind driver views to native owner leases (#30)`，只在 #28 gate 已真实满足时提交/勾选该任务。

## Task 7: 完整 SDK 验收、文档与下一阶段交接

**Files:** 创建 `docs/architecture/runtime-driver-sdk.md`，完善 Tasks 1–6 测试和 SDK presets。此任务不新增 Runtime/Loader/MySQL 实现，也不修改旧连接入口意义。

**Interfaces:** 文档只描述已经编译/测试的候选 SDK；清楚区分 bootstrap C ABI、Salts bundle、旧 facade ABI 4 与目标 facade ABI 5。SDK API 名称/字段顺序以实际头为准，差异先回填规范，不让文档和头分别演进。

- [ ] **RED：** 将每个 required field/callback/capability 的反例加入表驱动测试；把正确版本、合法尾字段和两连接正常生命周期作为正例。拿掉校验分支要确实触发一个断言；不要把 assert 放在代码永远不走的路径。
- [ ] **GREEN：** 仅修本 SDK 的遗漏，按 Task 1→6 顺序重跑 focused suites；不顺带接 MySQL，不禁用现有失败项。
- [ ] **SDK 独立验证：** 在 `orm/driver-sdk` 中，四个平台/config preset 在对应原生 OS 使用以下同结构命令。Linux Debug 的完整命令为：

```sh
cmake --preset sdk-linux-debug
cmake --build --preset sdk-linux-debug
ctest --preset sdk-linux-debug --show-only=json-v1
ctest --preset sdk-linux-debug --output-on-failure --no-tests=error
```

必须恰好包含 `orm_driver_prefix`、`orm_driver_descriptor`、`orm_driver_handshake`、`orm_driver_c_consumer`、`orm_driver_cpp_consumer` 五个 CTest 条目（TinyTest 内部案例数另外记录）。core-bound 两条是独立 suite，不计入这五条。SDK Debug 目标必须实际启用 ASan；SDK Release 目标不主动引入 sanitizer，所用 Salts/CRT 闭包也必须与该配置匹配，不能只靠本项目的编译选项推断整个闭包。Salts 的运行依赖用已声明 prefix 配置，不临时复制缺库或从其他 profile 补齐。

- [ ] **核心相邻与 full regression：** 在仓库根运行对应现有平台 preset 的 build、CTest。至少包含 `orm_driver_plan_view`、`orm_driver_owner_bridge`、`orm_sql_render`、`orm_cbind_publisher`、`orm_postgres_flow`、`orm_cpp_flow` 与 `orm_public_flow`；精确 CTest 名以该 head 的 `ctest --show-only=json-v1` 核对，缺项不以宽泛 regex 掩盖。full regression 失败则报告，不声明 #30 ready。
- [ ] **记录证据：** 记录 commit、OS/arch/config、编译器、Salts/SDK bundle、命令、exit code、CTest/TinyTest 数量、sanitizer 启用方式、MODULE exports、SDK-only link dependencies、已运行/未运行/失败/不支持项。文档/hash/syntax 检查不能计作这些测试的通过。没有 workflow 时执行脚本/人工结果也须可复验；需要 CI workflow 由明确执行任务增加，不从“创建计划”推断 CI 已运行。
- [ ] **Commit：** `docs(orm): document verified driver SDK and migration boundaries (#30)`。
- [ ] **Issue 交接：** #30 只在规范/SDK/只读 plan/所有权全部验收满足后关闭；#28 原有 TurboFlow acceptance 不被 SDK tests 替代；#31 的第一任务才是选定平台适配、真正加载两个独立 MODULE、registry 和 module pins；#32/#33/#34/#35/#36 全部保持原 gate。

## 验收追踪与风险

| 要求 | 本计划 | 后续 gate |
| --- | --- | --- |
| 版本/size/调用约定/短结构安全 | Tasks 1–4 | #31 必须对实际 OS-loaded 模块复验 |
| 必需 callback/能力/错误 out | Tasks 2–4 | #31 加载错误分类、#33 native SQLSTATE |
| 不暴露私有 plan/容器 | Tasks 2、4、5 | #32–#34 用真正数据库后端验证 |
| 借用期限、冻结、谁分配谁释放 | Tasks 3、5、6 | #28 原生 gate、#31 in-flight module pin |
| C/C++ 公开 SDK 使用 | Task 4、7 | #35 真正 package/CRT/ABI cutover |
| Runtime、多数据源、驱动动态选择 | 仅类型/接口设计准备 | #31、#32、#36 |
| MySQL 方言、类型、逐行数据、事务 | 不在本期实现 | #33 |
| core-only / 无数据库开发包 / native 闭包 | 仅 SDK-only 构建 | #34、#35、#36；旧 full-core 测试不证明 core-only |
| 同一二进制 hash、真实多驱动共存 | 本期不声称完成 | #36 |

**HIGH：** 普通 retain 不等于 operation lease；没有冻结计划/Publisher/worker 的真正所有权，不能让私有 plan view 跨调用生存。Task 6 被 #28 硬阻塞，禁止测试侧镜像计数冒充实现。

**HIGH：** descriptor 自报 size 不能证明地址可读；先校验调用者提供的实际字节数并使用真实短分配测试。已验证 descriptor 只在其 module pin 和不变性契约内有效；它不是恶意插件沙箱。

**HIGH：** 不修改 ABI 4 公共对象行为来让候选 ABI 1 测试变绿；3.0/ABI 5 的运行时切换必须与 #28/#31/#35 的真实实现同步。

**MED：** 先定义最小可验证 SDK，不在 #30 内建立另一个 executor、registry 或 loader。SDK-only source root 绕开顶层数据库依赖，仅说明 SDK 不需要 DB SDK，不能据此声称现有核心已完全解耦。

## 本次计划提交的验证记录

本次读取了固定 spec、PR #37/#28/#30、现有 presets、ORM CMake 测试注册和 SQL renderer tests；本地 spec blob 与远端已知 blob 一致。容器 `git ls-remote` 因 GitHub DNS 解析失败，CodeGraph 不可用，因此未进行完整 checkout、编译、原生模块加载或数据库测试。提交通过 GitHub connector 完成，具体计划文件 hash 和远端 blob 对应关系回填 PR/issue。

下一实际代码动作是 **Task 1 的 prefix/short-buffer RED**。本计划不是新的设计问卷，也不要求重做已批准架构；没有授权自动 merge。实施时逐任务 TDD、验证、提交，任何 gate 未满足保持未完成。

## 参考

- [既有测试注册](https://github.com/qigao/turbodb/blob/2ab6a3e2d3f2809881d3b49caccf9d4fd5d5ee15/orm/CMakeLists.txt)、[SQL plan 测试](https://github.com/qigao/turbodb/blob/2ab6a3e2d3f2809881d3b49caccf9d4fd5d5ee15/orm/tests/flow/orm_sql_render_test.c)、[CMakeUserPresets](https://github.com/qigao/turbodb/blob/2ab6a3e2d3f2809881d3b49caccf9d4fd5d5ee15/CMakeUserPresets.json)。
- [CMake MODULE](https://cmake.org/cmake/help/latest/command/add_library.html)：MODULE 不应作为 consumer 的链接 target。
- [CTest --no-tests=error](https://cmake.org/cmake/help/v3.21/manual/ctest.1.html)：避免无测试场景被误报通过。
- [#28](https://github.com/qigao/turbodb/issues/28)、[#30](https://github.com/qigao/turbodb/issues/30)、[PR #37](https://github.com/qigao/turbodb/pull/37)：实际代码/审批/测试状态以读取结果和提交证据为准。
