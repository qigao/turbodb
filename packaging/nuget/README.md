# TurboDB.Native

预编译 Release SDK，包含 ORM 核心、SQLite/PostgreSQL/MySQL/Redis/TidesDB 驱动，以及 MySQL 和 Redis 通用客户端。MongoDB 暂不发布。

每个平台的安装树位于 `sdk/linux-x64`、`sdk/windows-x64` 或 `sdk/android-arm64-v8a`。所有平台的驱动位于 `lib/turbodb/drivers`。Windows/Linux 另含 dbtools；Android 只发布库。

CI 和发布流程始终以 `Version="*"` 获取 Salts.Native 和 SaltsUtils.Native 的最新稳定版本，通过 `--no-cache --force-evaluate` 重新解析，不锁定版本、不生成依赖锁文件。每个安装树的 `turbodb-sdk-manifest.txt` 仅记录实际构建版本供诊断，不参与后续版本选择。Windows 安装树包含 vcpkg DLL，Salts/SaltsUtils 由对应 NuGet 包提供；运行时应把对应平台 SDK 的 `bin`（Windows）或 `lib`（Linux）加入库搜索路径。Android 应将使用的驱动及其共享库依赖随应用打包。

消费项目也应直接声明 `Salts.Native`、`SaltsUtils.Native` 的 `PackageReference Version="*"`，并在 restore 时使用 `--no-cache --force-evaluate`。NuGet 发布包中的传递依赖不能保证每次都选择最新版本，直接浮动引用才表达这一要求，参见 [NuGet 依赖解析规则](https://learn.microsoft.com/en-us/nuget/concepts/dependency-resolution)。

SDK 恢复和暂存步骤只负责下载、复制及导出路径。依赖是否可用由构建和测试验证，不检查 Salts/SaltsUtils 的版本或 ABI 数值。实际解析的包版本仅用于定位安装目录和记录构建信息。

CMake 入口为 `find_package(TurboDB CONFIG REQUIRED)`，不再提供独立的 `OrmConfig.cmake`。设置 `SALTS_ROOT`、`SALTS_UTILS_ROOT` 和 `TURBODB_ROOT` 指向匹配的平台安装树，以 `PATHS "$ENV{TURBODB_ROOT}" NO_DEFAULT_PATH` 查找 TurboDB。SDK 随包提供构建时匹配的 BoringSSL 库、头文件、CMake 配置和许可证；消费项目无需安装 OpenSSL/BoringSSL，consumer 的 vcpkg manifest 不包含 SSL 依赖。SaltsUtils 保留为运行时依赖，只有直接使用其 API 的客户端才需要查找它的 CMake package。

客户端按需链接 `Orm::C`、`Orm::Cpp`、`Orm::DriverABI`、`TurboDB::MySQL`、`TurboDB::Redis` 或 `TurboDB::SchemaABI`。驱动不通过链接自动加载：从 `TurboDB_DRIVER_DIR` 选择模块并调用 `orm_runtime_load_driver`。ORM 核心不直接依赖数据库客户端。

发布包将核心与五种驱动一起交付，客户端仍按需加载。当前仅支持 Driver ABI 2 / `TurboDb.Driver` 契约版本 4，旧 ABI 插件直接拒绝；升级时统一重编驱动 SDK 消费代码并成套替换核心与驱动。数据库格式不变。

从旧包迁移时，将 `find_package(Orm)` 改为 `find_package(TurboDB)`，保留原有 `Orm::*` 链接目标，并使用 `TurboDB_DRIVER_DIR` 定位模块。Salts/SaltsUtils 始终使用最新稳定版本；更新后通过构建、插件 ABI 检查和安装包消费测试验证兼容性，失败时直接报错，不降级到旧依赖。

完整的可构建示例见 [packaging/consumer](https://github.com/qigao/turbodb/tree/master/packaging/consumer)：它仅通过安装后的 TurboDB package 编译，加载五种驱动，并在 SQLite 内存数据库上执行 SQL。CI 在 Windows/Linux 运行该检查，Android 验证交叉编译和链接。

包内容断言位于 `packaging/tests/native_package_test.py`，使用 Python 标准库 unittest，覆盖三个平台的核心、驱动、公开头文件、随包 TLS 文件及 NuGet 依赖。打包后在仓库根目录运行：

```bash
TURBODB_NUPKG="dist/TurboDB.Native.${version}.nupkg" \
  python3 -m unittest discover -s packaging/tests -p '*_test.py' -v
```

ORM 核心不得直接链接数据库客户端的约束由 `orm_core_dependencies` CTest 用例验证，随 Windows/Linux 的常规测试运行。

普通 CI 和发布共用 `native-sdk.yml`：Windows/Linux 构建并运行 CTest、安装及消费测试，Android 交叉编译并链接消费示例。独立 E2E 仅覆盖 MySQL、PostgreSQL、Redis 真实服务行为，按相关路径触发。发布提交使用 `release: publish TurboDB package` 前缀，跳过重复的普通 SDK 构建；发布流程自身运行同一套测试。

依赖准备统一在 `.github/actions/setup-native`：读取共享 vcpkg NuGet 二进制缓存，并用 Actions cache 保留本仓库构建产生的本地二进制和 NuGet 包文件。缓存不含凭据配置或构建树，也不替代最新版本解析；vcpkg 仍按包 ABI 选择二进制。
