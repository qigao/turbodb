# TLS 测试证书

这些 first-party 夹具只供 `tidessql_mysql_tls` 正式测试使用，私钥是公开测试数据，
不得部署。证书使用 P256、ECDSA/SHA256；叶证书 SAN 为 `localhost`，信任根
为 `TidesSQL TLS test root`。有效期为 2026-01-01 至 2035-01-01，低于 GmSSL
`X509_VALIDITY_MAX_DAYS=3653` 的上限。CA 私钥在生成后丢弃。

证书由 Python cryptography 46.0.7 生成；运行和构建测试无需 Python 依赖。
更新时重新生成独立 CA 与服务端 P256 密钥，保留 CA BasicConstraints/pathLen=0、
CA keyCertSign/cRLSign、叶 digitalSignature/serverAuth、SAN=localhost，以及
两张证书的 SKI/AKI。不要延长至超过 GmSSL 的有效期上限，也不要关闭校验。
`p256-test-wrong-root-cert.pem` 的 subject 与正确根相同，但使用独立密钥，
用于验证错误信任根明确拒绝。更新后四项 TLS 测试必须同时通过：成功升级和
完整认证、错误身份拒绝、错误 CA 拒绝、握手超时。

[GmSSL X.509 契约](https://github.com/guanzhi/GmSSL/blob/v3.2.0/include/gmssl/x509_cer.h)，
[cryptography X.509 构建 API](https://cryptography.io/en/46.0.7/x509/reference/#x-509-certificate-builder)。
