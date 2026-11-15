# 构建与性能排障记录

## 2026-11-15 正式依赖与目录迁移验收

- 已核实最新正式发布：vcpkg-cache 0.1.3、OpenWTL 0.1.2、SaltsBase 0.1.8、
  CMeta 0.1.7、SaltsUtils 0.1.4、SaltsCNet 0.1.12。发布链接见根 README。
- SaltsCNet 0.1.12 的包名为 `SaltsCNet`，不能继续查找 `SaltsCore`。
  环境事实源统一为 `SALTS_CNET_VERSION` / `SALTS_CNET_ROOT`；本地 `.env`
  的相关版本和 SDK 根已与本仓库恢复回执对齐，其他机器设置保留。
- vcpkg-cache 预安装包不含 Studio 的 Scintilla/Lexilla；GmSSL 是静态
  Crypto/MySQL 的显式依赖。Windows 的现有 SDK 恢复入口现在显式执行项目
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
