# 构建与性能排障记录

## 2026-11-15 正式依赖与目录迁移验收

- 已核实最新正式发布：vcpkg-cache 0.1.3、OpenWTL 0.1.2、SaltsBase 0.1.8、
  CMeta 0.1.7、SaltsUtils 0.1.4、SaltsCNet 0.1.12。发布链接见根 README。
- SaltsCNet 0.1.12 的包名为 `SaltsCNet`，不能继续查找 `SaltsCore`。
  环境事实源统一为 `SALTS_CNET_VERSION` / `SALTS_CNET_ROOT`；本地 `.env`
  的相关版本和 SDK 根已与本仓库恢复回执对齐，其他机器设置保留。
- vcpkg-cache 预安装包不含 Studio 的 Scintilla/Lexilla；GmSSL 是静态
  Crypto/MySQL 的显式依赖。所有平台的现有 SDK 恢复入口现在显式执行项目
  manifest install，先准备完整依赖，成功后才发布恢复环境。
  vcpkg 构建端口所需编译器文件由该步骤内部调用共享 helper 准备，不向产品
  configure 导出 toolchain 路径，不把它们持久化到 `.env`。
- 2026-11-16 对照 TurboScript 与 http-services 的实际入口：二者均不加载
  外层 vcpkg toolchain，而是在根 CMake 中将预安装 triplet 加入
  `CMAKE_PREFIX_PATH`。TurboDB 已采用相同方式，移除了额外的
  `ConfigureLlvmMinGW.cmake` / `configure-llvm-mingw.py` 引导层。
  本地只配置 `LLVM_MINGW_ROOT`，编译器/target 仍由 presets 选择。
  清空所有派生 toolchain 变量、仅加载 `.env` 后，用户原命令
  `cmake --preset llvm-mingw-x64-release-user --fresh` 及该 preset 的完整 build
  均通过；Ninja 正常找到。日志见 `build/llvm-mingw-preinstalled-{configure,build}.log`。
  初次验证发现的四项运行时迁移失败已在下述 0.1.1 发布验收中修复。
- Lemon 使用该发布的 `Qigao.Lemon.Binary.1.0.0`，同时验证宿主 executable
  和 `share/lemon/lempar.c`，不能只恢复 re2c 或复用不同来源模板。
- 按用户确认移除 OpenWTL 子模块，改由 `app/CMakeLists.txt` 的 FetchContent
  下载固定版本、固定 SHA-256 的源码 ZIP。共享 SDK 下载器不接受该源码归档的
  codeload 重定向；源码依赖使用 CMake 原生归档校验，不修改 SDK 下载器的信任边界。
- 目录迁移后，ORM backends 必须由 `orm/CMakeLists.txt` 接回；模块 tests 的
  相对目录与源文件必须指向真实路径。`orm/types.h` 是共享头文件唯一源码，
  configure 时复制到构建目录的 `include/turbodb/types.h`，保持公开包含路径。

已执行的正式入口（同一 PowerShell 会话加载 `.env` / 恢复环境）：

```powershell
./cmake/ci/restore-native-sdks.ps1 -Rid windows-x64 -Profile windows-llvm-mingw-x64 -Local
cmake --fresh --preset native-release-user
cmake --build --preset native-release-user
./cmake/WithVs.cmd ctest --preset native-release-user -LE e2e --output-junit ctest-upstream.xml
cmake --build --preset install-native-release-user
```

恢复、完整 configure/build、正式 install 均成功。初次 CTest 88/92 通过；
0.1.1 发布验收修复后，本地 llvm-mingw CTest 92/92 通过。
没有跳过所选本地测试；外部数据库 e2e、MSVC、Linux/macOS、Android 由后续
正式发布 CI 分别核验，不把本地结果扩大为其他平台验证。
日志为 `build/native-release-{configure,build,tests,install}.log`，JUnit 为
`build/native-release-user/ctest-upstream.xml`。

用户确认 `drivers/mysql` 是有意的新标识，并授权恢复官方语料原文。
为 0.1.1 发布完成以下修复，未回退该新标识：

- **HIGH（已修复）** ORM ID 准入接受有界的斜杠命名空间，每段以小写字母
  开头；仍拒绝空段、路径遍历、反斜杠、嵌入 NUL 和非法字符。正式 registry
  测试覆盖这些边界，MySQL Plugin/共存测试通过，canonical ID 按实际 13 字节校验。
- **MED（已修复）** 官方语料来源路径恢复为 `mysql-test/...`，被误改的上游
  SQL 字符串、邮箱和提取片段恢复原文。`rebuild_mysql_corpus.py` 验证全部来源
  SHA-256 与行范围后重建 385 条语料，正式 corpus CTest 通过。
- **MED（已修复）** Studio 路径断言使用 `GetFullPathNameW` 返回的 Windows
  规范化分隔符，配置测试通过。
- 发布包按库文件名 `mysql` 查找 MySQL DLL，manifest 中仍记录新驱动 ID。
  ELF 闭包检查同时接受实际 SDK 的无版本 `libsalts.so` SONAME，仍要求每个
  NEEDED 与生产者 manifest 完全匹配。

用户要求发布 0.1.1 并推送全部代码，在推送时同步 GitHub CI variables：
`SALTS_BASE_VERSION=0.1.8`、`CMETA_VERSION=0.1.7`、
`SALTS_UTILS_VERSION=0.1.4`、`SALTS_CNET_VERSION=0.1.12`；
`VCPKG_CACHE_VERSION=0.1.3`。发布使用现有 `native-nuget-release.yml`，
由多平台构建、正式 CTest、install、包内容测试全部成功后发布 NuGet 和 GitHub Release。
本地发布验证日志为 `build/release-{configure,build,tests}.log`，
JUnit 为 `build/native-release-user/ctest-release.xml`。

首次发布 CI 的跨平台取证：

- vcpkg-cache 0.1.3 的 warm-up manifest 只在 Linux/Windows 预装 libpq。
  macOS/Android 必须在恢复阶段按本项目 manifest 显式安装，不能在 configure
  时借用系统 PostgreSQL 或跳过驱动。host triplet 按宿主 RID 明确选择。
- Linux 静态 SQLite 的 FTS5 对象依赖 libm；CMake 内置 FindSQLite3 的原始
  库路径不带完整依赖闭包。统一消费发布包的
  `unofficial::sqlite3::sqlite3` 导出 target，由生产者记录系统链接依赖。
- MSVC `/WX` 会把断言参数内复合字面量引起的 C4189 当作失败；边界测试改用
  具名的 counted view，保留嵌入 NUL 断言，不关闭警告。
- 上述修复后本地 llvm-mingw 正式 CTest 再次 92/92 通过；远端按相同正式流水线
  重新执行全部平台，失败即阻止打包和发布。
- 第二轮 CI 确认 Windows MSVC 的完整构建、正式测试和 SDK 暂存通过。Unix
  静态 libpq 还需 `pgcommon`、`pgport` 及系统库；根 CMake 使用选定安装目录
  内 `libpq.pc` 的静态导出元数据提供现有 `PostgreSQL::PostgreSQL` 契约，
  不借用宿主 PostgreSQL，也不复制 vcpkg wrapper 的库列表。
- Android NDK 27 的最终共享库显式链接 16 KiB 最大页大小，SDK 暂存对正式
  install manifest 内的 ELF 实际 LOAD segment 做对齐验收；这不代表设备运行通过。
- 第三轮 CI 的 Android 完整构建、install 和对齐暂存通过；Linux x64/ARM64
  完整构建通过，均为 86/87 项测试通过。剩余 CLI fixture 的临时文件前缀
  包含 `drivers/mysql` 中的斜杠，Unix mkstemp 将其解释为不存在的目录。
  文件名前缀改为 `dbtool-drivers-mysql-cli`，驱动 ID 与 CLI 参数保持不变。

## 发布 CI 的配置边界

- `setup-vcpkg-curl-cache` 在 CMake configure 之前解析 manifest；应用 overlay
  必须声明在 `vcpkg-configuration.json`，只写在 CMake preset 中会导致 CI
  报 `baseline does not contain an entry for port lexilla`。
- 链式加载 NDK `android.toolchain.cmake` 时使用其 `ANDROID_ABI`、
  `ANDROID_PLATFORM` 和 `ANDROID_STL` 输入。只设置 CMake 原生 Android
  变量可能仍按 NDK 默认 32 位 ABI 探测，继而拒绝已恢复的 64 位 SDK。
- PostgreSQL E2E fixture 只依赖公开 ORM 入口，错误输出通过公开结构和
  `fmt_text` 写入；不能依赖没有公开声明的 `orm_error_set`。
- 使用 `IPPROTO_TCP` 的 POSIX socket 适配器显式包含 `netinet/in.h`；
  glibc 的间接 include 会掩盖缺失，Android Bionic 不提供该间接声明。

## Redis / MySQL 协议扫描

- 短 RESP 回复和密集参数 SQL 不能为每个片段都进入外部向量函数。
  本次基准确认短片段的调用成本会抵消收益；短片段先就地扫描，长片段再批量处理。
  采用 `msvc-benchmark-user` 的正式 CTest 基准验证，具体数据和命令见 README。
- SaltsBase 0.1.5 的 `simd_reduce(..., SALTS_SIMD_REDUCE_BITMASK, ...)`
  使用数值 lane 描述符（此处 `simd_u8x16`），不能传 `simd_b8x16`；后者返回失败。
  失败直接向上传播，不切换备用实现。
- `tstr_new_len(NULL, n)` 会初始化存储。Redis 命令用 `tstr_new` +
  `tstr_reserve`，直接写入容量后再 `tstr_set_len_checked`，避免额外初始化大 payload。
- SQL 参数索引缩短后，后续输入和输出可能重叠。批量输出使用 `memmove`，
  保留原地改写；正式测试覆盖先删除索引、再复制长字符串与注释的场景。
- Windows 测量中出现过单批次数十毫秒的调度停顿，固定 CPU 也可能放大干扰。
  本次结果采用默认调度、三轮交替基线与优化版，每轮 200 × 1,024 次操作；
  原始日志与汇总保留在 `build/performance-{baseline,optimized}-*.log`
  和 `build/performance-comparison.json`。不把协议 CPU 基准写成数据库端到端吞吐。

## SaltsUtils 0.1.5 与 Crypto 依赖边界（2026-11-17）

- `.env` 只更新已有的 SaltsUtils 版本和根目录，选择已校验的正式 0.1.5
  LLVM-MinGW SDK；`.env.example` 同步版本。SDK 恢复回执在相邻 TurboRaft 的
  `build/dependencies/salts-utils-0.1.5-inputs/restore-receipt.json`，不提交 `.env`。
- 保留用户移除运行时复制框架、DriverABI 转发目标和旧 SDK 消费查找的改动。
  安装包仍需包含 `OrmTargets.cmake` 以公开 `Orm::C` / `Orm::Cpp`，
  并保留实际插件安装目录；依赖选择由消费方加载的环境负责。
- 消费端移除 GmSSL 的直接 package 查找和 target 链接，通过 `Salts::Crypto`
  使用加密能力。HIGH：当前 SaltsCNet 0.1.12 的导出接口引用
  `GmSSL::GmSSL`，包配置却未加载它。执行
  `cmake --fresh --preset native-release-user` 在 generate 阶段因此失败，
  日志为 `build/salts-utils-0.1.5-configure.log`，没有继续 build / CTest / install。
- HIGH：MySQL `mysql_native_password` 保留原协议所需的 SHA-1，当前上游尚无
  对应公开 Crypto API。直接头文件引用需随上游补齐封装后迁移，不能通过
  删除认证方式、手写摘要实现或修改已发布 SDK 绕过。

## CNet 0.1.13 / Utils 0.1.5 的环境选择（2026-11-17）

- 本地 `.env` 选择 `SALTS_CNET_VERSION=0.1.13` 与
  `SALTS_UTILS_VERSION=0.1.5`，SDK 根仍由同名 `*_ROOT` 选择；根 CMake 已经
  使用环境中的精确版本与路径做 `find_package(... EXACT ... NO_DEFAULT_PATH)`，
  本次不加入 CMake 默认版本、SDK 路径或隐式 `.env` 读取。
- CNet 原始 LLVM-MinGW SDK 取自正式发布的 `windows-x64` 资产，SHA-256 为
  `4c6183da4e04fd7c51113114f8449ccd156d5328ed26c907746384f8146b24c8`。
  发布清单、SHA256SUMS、资产大小、内嵌 SDK 清单和精确源码生产者
  [CI 44074223743](https://github.com/qigao/salts-cnet/actions/runs/44074223743)
  均已核对。版本独立目录为
  `build/dependencies/salts-cnet-0.1.13-windows-x64-4c6183da4e04`，回执保存在该目录。
- Utils 继续使用 `.env` 已选中的相邻 TurboRaft 正式 0.1.5 LLVM-MinGW SDK；
  再次核对原始 TAR 与发布清单 SHA-256、源码、profile 和生产者回执。
  SDK TAR 摘要为 `314ab3009235c1e9a15c62f460f4edc0aa66c47eab63658cb58ea5a6b91549de`。
- `.env.example` 补齐 SDK/生成工具根目录示例，README 同步版本、共享 Crypto
  运行库要求及父环境加载方式。保留用户已暂存的 CI helper 删除和依赖锁迁移。
  GitHub variables 当前仍为 CNet 0.1.12 / Utils 0.1.4，沿用此前约定，在代码合入时
  同步为 0.1.13 / 0.1.5，不提前改变远端构建环境。
- `cmake --fresh --preset native-release-user` 配置与生成通过，消除了旧 CNet
  导出接口要求 `GmSSL::GmSSL` 的问题。完整
  `cmake --build --preset native-release-user` 在
  `drivers/mysql/auth/auth.c:4` 失败：`gmssl/sha1.h` 不再由新 SDK 暴露。
  CNet 0.1.13 只导出共享 `Salts::Crypto`，没有公开 SHA-1 API；已有 MySQL
  `mysql_native_password` 仍直接调用该后端。HIGH：需确定上游公开 SHA-1 API
  或 MySQL 独立后端依赖的方向，不能通过删除认证能力或回退 SDK 掩盖。
  本轮未继续 CTest / install。日志为 `build/cnet-0.1.13-{configure,build}.log`。
- 补充核对用户指定的 `../salts-core/vendor`：当前 libecc 编译范围只有
  SHA-256/384/512 和 SHAKE256，Monocypher 不提供 SHA-1，不能用私有 vendor
  头文件替换 MySQL 的引用。本地上游在 v0.1.13 之后的提交 `1640e94`
  新增了公开 `crypto/symmetric.h` 与
  `salts_crypto_digest(SALTS_CRYPTO_SHA1, ...)`，内部委托 GmSSL，已有正式
  SHA-1 向量测试源码。该 API 尚不在本次选择的 0.1.13 SDK 中；待包含此提交
  的正式 SDK 发布后，可通过 `.env` 选择并迁移 MySQL，保持消费方只依赖
  `Salts::Crypto`。本次仅核对源码，没有将未发布接口写入 TurboDB。

## Crypto 公开接口扩展与 MySQL 接入（2026-11-17）

- 用户授权继续实现后，在相邻 `salts-core` 的既有 Crypto owner 中增加
  通用流式 Digest/HMAC、HKDF、AES-CBC/CTR、RSA-PSS salt/OAEP label、
  DER/PEM 导出及有迭代上限的加密 PKCS8 导入导出。API/生命周期及依赖限制
  见该仓库 `crypto/README.md` 和公开头文件。当前源版本为 0.1.14，正式
  0.1.13 SDK 不含这些接口；未改写已发布 SDK，也未引入直接 GmSSL 链接。
- MySQL 的 SHA-1 改用 `salts_crypto_digest`，失败沿用 INVALID 结果，
  native-password 与 caching-SHA2 的临时摘要缓冲区统一清理。
  正式边界测试中的独立协议向量保持通过，`drivers/mysql` ID 不变。
- 相邻 Crypto 完整 LLVM-MinGW build、101/101 正式 CTest 和 install 通过。
  本仓库显式在本次验证的父进程设置：
  `SALTS_CNET_VERSION=0.1.14`、
  `SALTS_CNET_ROOT=C:/projects/cpp/turbonet/salts-core/build/native-release-user/stage`，
  其余构建输入来自本地 `.env`。随后正式
  `cmake --fresh --preset native-release-user`、完整 build、
  `./cmake/WithVs.cmd ctest --preset native-release-user -LE e2e
  --output-on-failure --output-junit ctest-crypto-candidate.xml` 和
  `cmake --build --preset install-native-release-user` 均通过；CTest 为 92/92。
  日志为 `build/crypto-candidate-{configure,build,ctest,install}.log`。
- 首轮 90/92 的两项 MySQL Plugin 加载失败来自共享运行库部署。
  CMeta 的绝对路径 loader 使用 DLL_LOAD_DIR | DEFAULT_DIRS，普通 PATH
  不属于其信任源。明确将所选候选 SDK 的 `cnet.dll` / `salts_crypto.dll`
  部署到正式构建的应用目录后，两项测试及完整 92 项测试通过。
  原有文件与部署回执保存在 `build/crypto-candidate-runtime-backup/`。
  未恢复用户删除的自动运行库复制框架，未扩大插件 DLL 搜索策略。
- 本地 `.env` 仍选择正式 CNet 0.1.13 / Utils 0.1.5；本次候选版本/路径仅用于
  明确的源码集成验证。正式使用新接口仍需发布匹配 SDK 并同时更新版本和根目录。
  本轮未执行外部数据库 E2E、其他平台或 sanitizer，也未发布新版本。

## 十项 CI 矩阵与 SDK TAR 发布（2026-11-18）

- 触发：旧五项矩阵调用已暂存删除的恢复/暂存 helper，发布流程仍引用已删除
  的 NuGet 项目和包测试；sanitizer presets 写入 ENABLE_ASAN/UBSAN/TSAN，
  而项目只读取 ENABLE_SANITIZER_*，会造成未启用检查的错误通过。
- 将 .github/ci/sdk-matrix.json 作为十项资格验证的唯一矩阵入口：Linux Clang
  x64/ARM64、GCC x64、macOS ARM64、Windows MSVC/LLVM-MinGW x64、
  Android x64/ARM64、Linux ASan+UBSan、TSan。补齐 ASan preset 并修正变量名，
  不新增功能开关。八项 native 执行完整 build 与正式 CTest（排除已有 e2e
  标签）；两项 Android 执行完整 build/install 与安装产物 ELF 16 KiB 检查。
- 发布经用户确认改为十个独立 SDK TAR、发布清单及 SHA256SUMS，沿用
  tag/manual/release-commit 触发，复用同一完整构建图，不再依赖 NuGet 文件。
  Studio 的构建、PE/editor 检查及原生应用 artifact 纳入同一矩阵；
  不再保留重复的 Studio workflow。
- 准备步骤核对公开发布清单、producer CI、源码 tree、大小与 SHA-256，分发
  原始 SDK/编译器和 per-triplet locked 输入。恢复使用共享 vcpkg-cache 入口，
  未恢复被删除的 Python/PowerShell helper。runner 的 canonical vcpkg
  bootstrap 与产品 manifest 端口准备使用其正常的固定来源网络输入；
  产品 configure 继续只消费准备后的 prefix，不加载 vcpkg toolchain。
- 实际恢复发现两种合法上游结构：Utils 0.1.5 为 annotated tag，须先解引用
  到 commit 再核对 manifest；主机工具包路径为 tools/<host>，不带 payload/
  前缀（payload 是共享恢复器增加的安装外层）。CMeta 0.1.7 SDK 为静态库，
  没有 bin 目录；Windows 部署遍历所选 SDK 根的 DLL，vcpkg 只选 release bin，
  同名不同摘要立即失败，不依赖 PATH 绕过插件 loader 的信任边界。
- 已验证：actionlint、全部内联 PowerShell 语法、十项 configure/build/install/
  test preset 对应关系；Base 0.1.8、CMeta 0.1.7、Utils 0.1.5、CNet 0.1.15
  在十项配置中的 40 项 SDK 选择均通过正式清单/producer/tree/大小/摘要校验。
  Re2c/Lemon 从正式发布资产按锁恢复成功。
- 本机使用上述正式 SDK 与已准备的 vcpkg prefix：
  cmake --fresh --preset ci-llvm-mingw-x64-release-user；
  cmake --build --preset ci-llvm-mingw-x64-release-user --parallel 4；
  cmake/WithVs.cmd ctest --preset ci-llvm-mingw-x64-release-user -LE e2e；
  cmake --build --preset install-ci-llvm-mingw-x64-release-user 均通过，
  正式 CTest 为 92/92。实际执行 workflow 的 Windows runtime 部署、Studio
  PE 检查和 SDK TAR 归档步骤，核对 provenance、CMake package 与许可证。
  日志在 build/workflow-review/；.env 的机器设置及 SDK 选择未改写。
- 未验证：新 runner 上的完整 dependency bootstrap、其他九项实际构建、
  sanitizer 运行、Android 设备、外部 DB E2E、远端打包/发布。合入时同步
  GitHub variables 为所选 CNet 0.1.15 / Utils 0.1.5，沿用不提前影响旧代码
  的约定；本轮未推送、打 tag 或发布 TurboDB。

## 本地 CNet SDK 仍为 0.1.13 的缺头文件错误（2026-11-18）

- 触发：drivers/mysql/auth/auth.c 包含 crypto/symmetric.h 时编译失败。
  .env 和 llvm-mingw-x64-release-user 的 SaltsCNet_DIR / compile_commands.json
  均仍指向 0.1.13；此前 0.1.15 验证使用进程级覆盖，并未改变本地选择。
  MySQL target 已正确链接 Salts::Crypto，不能通过追加源码目录或直接链接
  GmSSL 修复 SDK 版本不匹配。
- 已按正式发布清单、恢复回执、原始 TAR 大小/SHA-256 校验本地 0.1.15 SDK，
  并逐文件核对 73 个安装文件与原始归档。只更新 .env 的 SALTS_CNET_VERSION
  和 SALTS_CNET_ROOT，其余机器设置与凭据保留，.env 仍被 Git 忽略。
- 根 CMake 在配置边界声明 API 下限 0.1.14，旧 SDK 明确失败并提示同步版本、
  根目录及 --fresh；该下限不替代环境选择的精确版本。公开 v0.1.14 头文件
  已包含本次 MySQL 使用的 SHA-1 digest / secret-clear API。
- 正式验证：0.1.13 的 llvm-mingw-x64-release-user configure 按预期失败；
  加载更新后的 .env 后，该 preset 的 --fresh configure 与完整 build 通过；
  ci-llvm-mingw-x64-release-user 的 --fresh configure、完整 build 和
  cmake/WithVs.cmd ctest --preset ci-llvm-mingw-x64-release-user -LE e2e 通过，
  为 92/92。日志为 build/crypto-header-*.log，JUnit 为
  build/ci-llvm-mingw-x64-release-user/ctest-crypto-header.xml。
- 一次正向 configure 曾在编译器探测阶段报告 CMAKE_C_COMPILER not set；
  失败日志产生后，同目录 CMakeCache.txt 与 build.ninja 又继续更新。
  推论：存在编辑器等外部配置任务并发操作同一构建目录；未更换工具链或
  屏蔽探测。无并发任务时单独完成正向 configure/build。原失败日志保留在
  build/crypto-header-concurrent-configure.log，不能将其归因为 Crypto 头文件。

## 首次推送后 CI 仍使用旧变量与 Windows 路径错误（2026-11-18）

- 事实：ecff5e8 的 push run 44128210288 创建于 16:39:16Z；
  SALTS_CNET_VERSION / SALTS_UTILS_VERSION 分别在 16:39:19Z / 16:39:22Z
  才更新。prepare 日志明确选择 CNet 0.1.12 / Utils 0.1.4，并生成对应的
  原始 SDK 输入，后续 configure 因 API 最低版本 0.1.14 正确失败。
  当前 repository variables 已为 0.1.15 / 0.1.5，修改变量不改变该旧 run
  已固定的环境和准备产物。SDK 升级应先同步变量再触发 CI；若先推送，
  同步后须启动完整的新 run，不能只重用旧 prepare 的 artifact。
- 同一 run 的 LLVM-MinGW lane 在资源编译器探测阶段更早失败：
  setup-llvm-mingw 返回合法 Windows 原生路径 D:\a\_temp\...，preset 将
  CMAKE_RC_COMPILER 当作未标明类型的字符串传入，生成的 CMakeRCCompiler.cmake
  把反斜杠解释为转义。此问题独立于 CNet 版本。
- 将 LLVM-MinGW 的 C/C++、AR、RANLIB、RC 路径声明为 FILEPATH cache 值，
  由 CMake 正常规范化路径，不新增路径猜测、工具链切换或 SDK 备用来源。
  用本机 LLVM_MINGW_ROOT=C:\tools\llvm-mingw 在正式 CI preset 中复现
  Invalid character escape；修正后同一反斜杠输入的 --fresh configure、
  完整 build、正式 CTest 92/92 通过。日志为 build/ci-backslash-*.log。
- 推送修正前再次核对远端 CNet 0.1.15 / Utils 0.1.5，重新触发完整十项
  资格验证；远端结果以后续实际 run 为准，不把本机或旧 run 的结果视为通过。

## 合并 CI 入口并移除自动发布（2026-11-18）

- 按用户本轮清理决定，删除独立 native-sdk-release.yml；将
  driver-sdk-package.yml 包装入口与 native-sdk.yml 复用流程合并为唯一
  .github/workflows/ci.yml。此决定取代前面关于自动发布十个 TAR 的计划。
  保留 PR/master push/manual dispatch、十项矩阵、完整构建/正式 CTest、
  Android ELF 检查、Studio PE 检查及 SDK/Studio/test artifacts；
  移除 release-commit 跳过逻辑、workflow_call 版本参数和自动 tag/release/package
  发布。发布改为手动，历史 release 与 run 记录不删除。
- 05e6442 的 run 44129844126 已明确使用 CNet 0.1.15 / Utils 0.1.5。
  Linux Clang x64/ARM64、GCC x64、两项 Windows、两项 Android 共七项已通过。
  两项 Clang sanitizer 在 turbo_orm.so 链接阶段因 -z defs 拒绝 sanitizer
  runtime imports 而失败；这与旧 SDK 选择已无关。
- Clang 的 sanitizer runtime 由最终 executable 提供，符合
  https://clang.llvm.org/docs/AddressSanitizer.html#usage。Sanitizers.cmake
  派生当前编译器/runtime 链接事实，仅在 Clang instrumentation 模式下
  省略 ORM core / MySQL module 的 -z defs；普通生产 Linux 保持严格闭包，
  MySQL 的 --exclude-libs,ALL 保留，不关闭 sanitizer 或放宽运行时错误。
- 本地检查通过：唯一 workflow 的 actionlint、18 个 PowerShell 步骤语法、
  十项矩阵 / 八项 native test lane；使用反斜杠 LLVM_MINGW_ROOT 的正式 CI
  preset --fresh configure、完整 build、CTest 92/92 通过。日志为
  build/ci-cleanup-*.log。Sanitizer 链接和其他平台的最终结果需以本次新 CI
  run 为准；旧失败日志保留，不将旧 run 的部分通过写为十项全部通过。
