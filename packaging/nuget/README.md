# TurboDB.Native

预编译 Release SDK，包含 ORM 核心、SQLite/PostgreSQL/MySQL 驱动，以及 MySQL 和 Redis 通用客户端。Redis 是独立客户端，不属于 ORM 驱动。2.3.4 发布包不包含 TidesDB、TidesSQL 或 `tidessqld`。

每个平台的安装树位于 `sdk/linux-x64`、`sdk/linux-arm64`、`sdk/macos-arm64`、`sdk/windows-x64` 或 `sdk/android-arm64-v8a`。所有平台的驱动位于 `lib/turbodb/drivers`。Windows/Linux/macOS 另含 dbtools；Android 只发布库。

正式发布和普通 CI 均以 `Version="*-*"` 获取 Salts.Native 和 SaltsUtils.Native 的最新发布版本，包括 RC，不限定版本系列。每轮通过 `--no-cache --force-evaluate` 重新解析，不生成依赖锁文件；实际版本以本轮 restore 输出为准。打包检查五个平台的生产端版本一致及 Linux SONAME 依赖闭包。每个安装树的 `turbodb-sdk-manifest.txt` 记录实际构建版本供诊断。macOS 使用 AppleClang；恢复的 SDK 必须支持该工具链，构建或测试失败会停止发布。Windows 安装树只从 vcpkg 复制 SQLite/PostgreSQL 驱动所需的 `sqlite3.dll` 和 `libpq.dll`；libpq 不含 SSL，TLS 由 Salts::CNet 提供。Salts/SaltsUtils 由对应 NuGet 包提供；运行时应把对应平台 SDK 的 `bin`（Windows）或 `lib`（Linux/macOS）加入对应平台的运行时库搜索路径。Android 应将使用的驱动及其共享库依赖随应用打包。

正式包的消费项目应直接声明 `Salts.Native` 和 `SaltsUtils.Native` 的 `PackageReference Version="*-*"`，在 restore 时使用 `--no-cache --force-evaluate`，并核对安装树 manifest 中的运行库 ABI。TurboDB 包不嵌入这两个 GitHub 包的传递依赖，直接浮动引用表达每轮获取最新版本（含预发布版）的要求，参见 [NuGet 浮动版本规则](https://learn.microsoft.com/en-us/nuget/concepts/dependency-resolution#floating-versions)。

SDK 恢复和暂存步骤负责下载、复制及导出路径，并记录实际解析的包版本；ELF 平台另记录恢复的 Salts 运行库中的 `DT_SONAME`，供打包时比较 TurboDB 的 `DT_NEEDED`。SONAME 由生产端库定义，不能从 NuGet 包版本推导，例如 Salts.Native 2.2.0 的 SONAME 是 `libsalts.so.2`。依赖是否可用由构建和正式测试验证，发布流程不排除预发布 SDK。

CMake 入口为 `find_package(TurboDB CONFIG REQUIRED)`，不再提供独立的 `OrmConfig.cmake`。设置 `SALTS_ROOT`、`SALTS_UTILS_ROOT` 和 `TURBODB_ROOT` 指向匹配的平台安装树，以 `PATHS "$ENV{TURBODB_ROOT}" NO_DEFAULT_PATH` 查找 TurboDB。MySQL TLS 由 Salts::CNet 管理，认证摘要使用 Salts 的 provider-neutral crypto API；TurboDB SDK 不再随包携带 OpenSSL/BoringSSL。SaltsUtils 保留为运行时依赖，只有直接使用其 API 的客户端才需要查找它的 CMake package.

客户端按需链接 `Orm::C`、`Orm::Cpp`、`Orm::DriverABI`、`TurboDB::MySQL`、`TurboDB::Redis` 或 `TurboDB::SchemaABI`。驱动不通过链接自动加载：从 `TurboDB_DRIVER_DIR` 选择模块并调用 `orm_runtime_load_driver`。ORM 核心不直接依赖数据库客户端。

发布包将核心与三种驱动一起交付，客户端仍按需加载。当前仅支持 Driver ABI 2 / `TurboDb.Driver` 契约版本 4，旧 ABI 插件直接拒绝；升级时统一重编驱动 SDK 消费代码并成套替换核心与驱动。数据库格式不变。TidesDB ORM 驱动及其 `orm_tidesdb.h` 已移除。2.3.4 起发布配置同时关闭 TidesSQL 及其 TidesDB 引擎，不交付相关库、头文件、CMake package、daemon 或私有 zstd archive。需要这些组件时须从源码显式开启构建；升级时使用新的 SDK 安装目录，避免沿用旧组件。

从旧包迁移时，将 `find_package(Orm)` 改为 `find_package(TurboDB)`，保留原有 `Orm::*` 链接目标，并使用 `TurboDB_DRIVER_DIR` 定位模块。正式包使用最新发布的 Salts/SaltsUtils SDK，包括预发布版。Driver 只随包部署，不自动加载；应用按需显式调用 `orm_runtime_load_driver()`。缺失依赖、错误 module path 或 ABI 不匹配直接失败，不提供 consumer harness、兼容回退或旧依赖降级。

包内容断言位于 `packaging/tests/native_package_test.py`，使用 Python 标准库 unittest，覆盖五个平台的 ORM 核心、驱动、dbtools、公开头文件及 NuGet 依赖，并拒绝 TidesDB/TidesSQL SDK 文件。测试还精确约束 Windows `bin` 的 DLL 集合，防止把 vcpkg 的无关工具库带入发布包。打包后在仓库根目录运行：

```bash
TURBODB_NUPKG="dist/TurboDB.Native.${version}.nupkg" \
  python3 -m unittest discover -s packaging/tests -p '*_test.py' -v
```

ORM 核心不得直接链接数据库客户端的约束由 `orm_core_dependencies` CTest 用例验证，随 Windows/Linux 的常规测试运行。

普通 CI 和发布共用 `native-sdk.yml`，各平台 job 按 configure → build → test → install 分成独立步骤；失败后不继续安装、暂存或上传该平台 SDK，其他平台继续运行。Windows x64、Linux x64/ARM64 和 macOS ARM64 在原生 runner 上通过对应 user preset 运行正式 CTest。Android ARM64 只交叉编译和安装；仓库未配置 Android 设备或模拟器 runner，因此不在宿主上运行 Android 测试。CI 已删除 MySQL/PostgreSQL 外部服务验证 job 和专用 E2E presets；发布只运行交付组件的原生单元测试与包内容测试，不宣称本轮完成外部数据库 E2E。发布提交使用 `release: publish TurboDB package` 前缀，跳过重复的普通 SDK 构建；发布流程自身运行同一套测试。

依赖准备统一在 `.github/actions/setup-native`：读取共享 vcpkg NuGet 二进制缓存，并用 Actions cache 保留本仓库构建产生的本地二进制和 NuGet 包文件。缓存不含凭据配置或构建树，也不替代最新版本解析；vcpkg 仍按包 ABI 选择二进制。
