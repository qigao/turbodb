# Android 构建、安装包、真机测试与 LLDB

TurboDB 使用统一的 CMake Presets 命名和输出目录，从 Windows 主机通过
Android NDK 交叉编译，并使用 `tools/android-test.ps1` 将单个测试 target
部署到 USB 或 WiFi ADB 设备运行。

## 当前约定

| 用途 | 值 |
|---|---|
| ARM64 Release preset | `android-arm64-v8a-release-win` |
| ARM64 Debug preset | `android-arm64-v8a-debug-win` |
| Release build tree | `build/android-arm64-v8a-release` |
| Debug build tree | `build/android-arm64-v8a-debug` |
| Android package | `C:/projects/cpp/external/pkgs/turbodb-android` |
| 设备工作目录 | `/data/local/tmp/turbodb-tests` |
| 最低 Android API | 24 |
| STL | `c++_shared` |

共享的 `presets/AndroidPresets.json` 定义 arm64-v8a、armeabi-v7a、x86_64
和 x86 的 Debug/Release configure、build、install presets。
`CMakeUserPresets.json` 只保存本机 NDK、Ninja、vcpkg、宿主生成器和依赖路径。

Android 的 TurboUtils 与 TurboDB 必须使用相同 ABI 和构建类型。
ARM64 Release 默认读取：

- `C:/projects/cpp/turbonet/turbo-utils/build/android-arm64-v8a-release`

## 前置条件

- PowerShell 7、CMake、Ninja、Android SDK Platform Tools 和 NDK。
- `CMakeUserPresets.json` 中的本机路径存在。
- 同 ABI/配置的 TurboUtils Android build tree 已配置并构建。
- ADB 设备状态为 `device`，且设备 ABI 与 preset 一致。
- TurboDB 的宿主 lemon 已构建。当前 Windows 路径为
  `build/Msvc-Release/bin/lemon.exe`；re2c 使用宿主机可执行文件。

检查设备：

```powershell
& "$env:LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe" devices -l
```

Android 11 及以上可使用无线调试，先按
[Android 官方无线调试说明](https://developer.android.com/studio/run/device.html#wireless)
完成配对和连接。

## 配置、构建与安装

ARM64 Release：

```powershell
cmake --preset android-arm64-v8a-release-win
cmake --build --preset android-arm64-v8a-release-win --parallel
cmake --build --preset install-android-arm64-v8a-release-win --parallel
```

测试 target 在 Android 下保留，但从默认 `ALL` 排除。全量构建不会编译所有
TurboDB 测试；runner 会显式构建指定 target。桌面平台既有默认行为不变。

Debug：

```powershell
cmake --preset android-arm64-v8a-debug-win
cmake --build --preset android-arm64-v8a-debug-win --parallel
```

安装内容包含 headers、库、`TurboDBConfig.cmake`、`TidesDBConfig.cmake` 和导出 targets。
消费项目仍需提供同 ABI/配置的 TurboUtils、xxHash 和 zstd；
package 不复制这些依赖。

消费端示例：

```cmake
find_package(TurboDB CONFIG REQUIRED)
find_package(TidesDB CONFIG REQUIRED)

target_link_libraries(app PRIVATE
    TurboDB::Redis
    TidesDB::tidesdb)
```

消费项目的 `CMAKE_PREFIX_PATH` 应包含 TurboUtils、TurboDB package
以及对应 vcpkg Android triplet，不能混入 Windows package。

## WiFi 真机测试

```powershell
./tools/android-test.ps1 test_redis_cflow `
  -Serial "adb-38101FDJG00AVU-Rx6MV9._adb-tls-connect._tcp" `
  -Tap `
  -LibraryDirectory @(
    "C:/projects/cpp/turbonet/turbo-utils/build/android-arm64-v8a-release/bin"
  )
```

只有一台在线设备时可省略 `-Serial`。PowerShell 中多个
`-LibraryDirectory` 必须作为数组传入；不要把逗号分隔的路径作为一个字符串。

TinyTest 与已有产物：

```powershell
./tools/android-test.ps1 test_redis_cflow -Filter "RESP stream" -Tap
./tools/android-test.ps1 manifest_tests -JUnit artifacts/manifest_tests.android.xml
./tools/android-test.ps1 manifest_tests -TestArgument @("--list", "--no-color")
./tools/android-test.ps1 manifest_tests -NoBuild
```

需要 fixture 时使用 `-Data`；只有单个额外动态库时使用 `-Library`。

## runner 的事实源

runner 不解析 CTest XML，也不从 CMake 输出文本猜测路径：

| 信息 | 来源 |
|---|---|
| build/configure preset | Presets JSON 的 include/inherits |
| build tree | 合并后的 configure preset `binaryDir` |
| target 类型与 artifact | CMake File API codemodel |
| triplet 与工具链 | `CMakeCache.txt` |
| ABI 与动态依赖 | ELF header 和 `DT_NEEDED` |
| 测试结果 | 设备退出码及可选 TinyTest JUnit |

脚本验证设备 ABI，递归部署非系统 `.so`，设置设备端 `LD_LIBRARY_PATH`，
并返回真实测试退出状态。缺少库、路径或元数据时立即失败。

字段语义参见
[CMake Presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)
和 [CMake File API](https://cmake.org/cmake/help/latest/manual/cmake-file-api.7.html)。

## LLDB 调试

```powershell
./tools/android-test.ps1 test_redis_cflow -Lldb

./tools/android-test.ps1 test_redis_cflow `
  -NoBuild -Lldb `
  -LldbCommand @(
    "breakpoint set --name main",
    "process continue",
    "thread backtrace",
    "process continue",
    "quit"
  )
```

脚本从当前 NDK 选择匹配 ABI 的 `lldb-server`，通过 ADB TCP forward 连接，
加载主机未剥离 ELF 符号，并在结束时清理资源。它不要求 `adb root`，
适用于普通 Pixel user build。

实现思路与 AOSP
[gdbclient.py](https://android.googlesource.com/platform/development/+/refs/heads/main/scripts/gdbclient.py)
一致，但针对独立 CMake/NDK 项目使用 File API 查找 artifact。

## 在其他项目复用

其他 CMake 项目可以复用相同 preset 名和 build tree；名称不是 runner 的硬编码条件。
复制 runner 后需满足：

- 脚本位于项目根目录的 `tools/`。
- build preset 能解析 configure preset 和 `binaryDir`。
- configure preset 能解析 NDK 路径。
- 目标为 Android ELF `EXECUTABLE`。
- configure 后生成 CMake File API codemodel。
- 测试参数兼容 TinyTest，或通过 `-TestArgument` 传递。

新增本机或 NDK 版本时只更新 `CMakeUserPresets.json`；共享
`presets/AndroidPresets.json` 不写个人绝对路径。脚本只读取 preset，不自动回写。

## 已复验结果

本轮未连接 Android 设备，因此未复跑真机测试；以上命令和 runner 的失败语义已按
TurboDB 当前 target 与安装包名称更新，仍需在目标 ABI 的设备上验证部署和 LLDB。
