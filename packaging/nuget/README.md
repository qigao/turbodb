# TurboDB.Native

预编译 Release SDK，包含 ORM 核心、SQLite/PostgreSQL/MySQL 驱动，以及 MySQL 和 Redis 通用客户端。Redis 是独立客户端，不属于 ORM 驱动。2.3.4 发布包不包含 TidesDB、TidesSQL 或 `tidessqld`。

每个平台的安装树位于 `sdk/linux-x64`、`sdk/linux-arm64`、`sdk/macos-arm64`、`sdk/windows-x64` 或 `sdk/android-arm64-v8a`。所有平台的驱动位于 `lib/turbodb/drivers`。Windows/Linux/macOS 另含 dbtools；Android 只发布库。

构建使用环境精确选定的 SaltsBase、CMeta、SaltsUtils 与 CNet 正式版；变量和恢复步骤见根 README 的[依赖版本说明](../../README.md#upstream-dependency-versions)。SDK 来自经 SHA-256 校验的 GitHub Release 资产。NuGet 包不嵌入或隐式恢复上游 SDK。调用方提供 `SALTS_BASE_ROOT`、`CMETA_ROOT`、`SALTS_UTILS_ROOT` 和 `SALTSCORE_ROOT`，运行时使用匹配 SDK 的 `bin` 或 `lib`。

CMake 入口为 `find_package(TurboDB CONFIG REQUIRED)`。导出配置从显式根必需查找匹配版本的 `SaltsBase`、`CMeta`、`SaltsUtils` 和 `SaltsCore`。MySQL TLS 使用 `Salts::CNet`，SHA-256 使用 `Salts::Crypto`，`mysql_native_password` 的 SHA-1 使用已接入的 GmSSL。Windows 包保留 SQLite/PostgreSQL 驱动所需的 `sqlite3.dll` 和 `libpq.dll`。

客户端按需链接 `Orm::C`、`Orm::Cpp`、`Orm::DriverABI`、`TurboDB::MySQL`、`TurboDB::Redis` 或 `TurboDB::SchemaABI`。驱动不通过链接自动加载：从 `TurboDB_DRIVER_DIR` 选择模块并调用 `orm_runtime_load_driver`。ORM 核心不直接依赖数据库客户端。

发布包将核心与三种驱动一起交付，客户端仍按需加载。当前仅支持 Driver ABI 2 / `TurboDb.Driver` 契约版本 4，旧 ABI 插件直接拒绝；升级时统一重编驱动 SDK 消费代码并成套替换核心与驱动。数据库格式不变。TidesDB ORM 驱动及其 `orm_tidesdb.h` 已移除。2.3.4 起发布配置同时关闭 TidesSQL 及其 TidesDB 引擎，不交付相关库、头文件、CMake package、daemon 或私有 zstd archive。TidesDB 已从源码树移除，TidesSQL 源码仅归档；升级时使用新的 SDK 安装目录，避免沿用旧组件。

从旧包迁移时，将 `find_package(Orm)` 改为 `find_package(TurboDB)`，保留原有 `Orm::*` 链接目标，并使用 `TurboDB_DRIVER_DIR` 定位模块。正式包使用环境精确选定的上游正式版 SDK。Driver 只随包部署，不自动加载；应用按需显式调用 `orm_runtime_load_driver()`。缺失依赖、错误 module path 或 ABI 不匹配直接失败，不提供 consumer harness、兼容回退或旧依赖降级。

包内容断言位于 `packaging/tests/native_package_test.py`，使用 Python 标准库 unittest，覆盖五个平台的 ORM 核心、驱动、dbtools、公开头文件及 NuGet 依赖，并拒绝 TidesDB/TidesSQL SDK 文件。测试还精确约束 Windows `bin` 的 DLL 集合，防止把 vcpkg 的无关工具库带入发布包。打包后在仓库根目录运行：

```bash
TURBODB_NUPKG="dist/TurboDB.Native.${version}.nupkg" \
  python3 -m unittest discover -s packaging/tests -p '*_test.py' -v
```

ORM 核心不得直接链接数据库客户端的约束由 `orm_core_dependencies` CTest 用例验证，随 Windows/Linux 的常规测试运行。

普通 CI 和发布共用 `native-sdk.yml`，各平台 job 按 configure → build → test → install 分成独立步骤；失败后不继续安装、暂存或上传该平台 SDK，其他平台继续运行。Windows x64、Linux x64/ARM64 和 macOS ARM64 在原生 runner 上通过对应 user preset 运行正式 CTest。Android ARM64 只交叉编译和安装；仓库未配置 Android 设备或模拟器 runner，因此不在宿主上运行 Android 测试。CI 已删除 MySQL/PostgreSQL 外部服务验证 job 和专用 E2E presets；发布只运行交付组件的原生单元测试与包内容测试，不宣称本轮完成外部数据库 E2E。发布提交使用 `release: publish TurboDB package` 前缀，跳过重复的普通 SDK 构建；发布流程自身运行同一套测试。

依赖准备统一在 `.github/actions/setup-native`：读取共享 vcpkg NuGet 二进制缓存，并用 Actions cache 保留本仓库构建产生的本地二进制和 NuGet 包文件。缓存不含凭据配置或构建树，也不替代最新版本解析；vcpkg 仍按包 ABI 选择二进制。
