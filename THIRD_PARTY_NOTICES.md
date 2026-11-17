# 第三方材料说明

## OpenWTL

Studio 通过固定 SHA-256 的正式发布源码归档使用
[qigao/OpenWTL 0.1.2](https://github.com/qigao/OpenWTL/releases/tag/v0.1.2)，
固定提交 `311b2dc361eea42cf6c821289133a99713298a74`，链接公开 target
`OpenWTL::WTL`。归档不含本地补丁，由 `app/CMakeLists.txt` 的 CMake FetchContent
恢复到 `build/<preset>/_deps/openwtl-src`；URL 和 SHA-256 是唯一事实源。

- 项目代码与构建集成：Apache-2.0，见归档根目录 `LICENSE`。
- OpenATL 基础：MIT，见归档内 `third_party_licenses/OPENATL-MIT.txt`。
- WTL 10.0.10320：MS-PL，见归档内 `wtl/MS-PL.txt`；来源记录见同目录 `SOURCE.md`。

上游版权与许可证文件随源码归档完整保留；不再依赖 Microsoft ATL 或独立 vcpkg WTL 包。

## GmSSL 密码库

当前 MySQL client 认证通过 SaltsCNet 的公开 `Salts::Crypto` 接口使用
GmSSL 后端，不直接包含或链接 GmSSL。来源：
[guanzhi/GmSSL](https://github.com/guanzhi/GmSSL/tree/v3.2.0)，许可 Apache-2.0；
版本、补丁及后端许可由所选 SaltsCNet SDK 管理，部署该 SDK 的运行库时
一并保留其许可材料。TurboDB SDK TAR 不重新分发这些上游运行库。
未纳入根构建图的私有 TidesSQL 源码仍有直接 GmSSL 引用，不能将其视为
当前 TurboDB 的公开依赖或已验证模块。

## MySQL 8.4.0 测试语料

`sqlparser/mysql-spec/upstream/` 保存 MySQL 官方测试输入及原始许可证；
`sqlparser/mysql-spec/cases/` 是按 `manifest.json` 指定行范围提取的原文片段。
这些第三方材料保留上游许可，不适用仓库 first-party 代码的 Apache-2.0 许可。

- 来源：[mysql/mysql-server](https://github.com/mysql/mysql-server/tree/dc86e412f18b36ce271f791026714e8caa0ec919)
- 版本：`mysql-8.4.0`
- Commit：`dc86e412f18b36ce271f791026714e8caa0ec919`
- Copyright (c) 2000, 2024, Oracle and/or its affiliates.（上游 README）
- 许可：GPLv2 及上游 LICENSE 所列附加许可；完整原文见
  [upstream/LICENSE](sqlparser/mysql-spec/upstream/LICENSE)。
- 本地处理：保留完整来源文件；选取独立 SQL 语句，逐字节复制，不改写 SQL；
  来源行号、偏移、哈希、上游错误预期及本地覆盖缺口单独记录。
- 用途：仅供解析器测试读取；不编译、链接或安装进 `TurboDB::SqlParser`。
