# 第三方材料说明

## GmSSL 密码库

MySQL client 认证和私有 TidesSQL server 密码边界通过 vcpkg 的 `GmSSL::GmSSL`
接入 GmSSL 3.2.0，不复制上游源码。来源：[guanzhi/GmSSL](https://github.com/guanzhi/GmSSL/tree/v3.2.0)，
许可 Apache-2.0；安装包保留上游 `share/gmssl/copyright`。项目依赖固定在
qigao/vcpkg-cache commit `1da9c439a2380a8752e88046ca0c5ae159991e03` 的 gmssl port，
该 port 包含 Salts CNet 所需 TLS 适配补丁与 SHA1/SHA2/AES/P256 构建选项；
补丁和版本元数据由 registry 管理。其他 registry 包的原 baseline 不变。

## Zstandard 压缩库

TidesDB 通过既有 vcpkg manifest 和 registry 使用
[facebook/zstd](https://github.com/facebook/zstd) 的静态库。SDK 的静态 TidesDB
配置随包提供最终链接所需的私有 archive，按 BSD-3-Clause 许可使用，并保留
生产端 vcpkg 包的完整许可证文件 `share/tidesdb/zstd/copyright`；版本及构建补丁
继续由 `vcpkg-configuration.json` 所指的 registry 管理。

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
