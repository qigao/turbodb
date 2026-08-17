# libpq BoringSSL overlay

This overlay is copied from the vcpkg `libpq` 16.9 port at baseline
`b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01`.

The overlay requires BoringSSL directly; callers do not select a TLS provider
feature or define a provider identity macro. PostgreSQL compiles its compatible
TLS implementation while BoringSSL supplies the headers, CMake metadata, and
libraries. The Windows build uses BoringSSL's OpenSSL 1.1.1 compatibility
version and selects
libpq's
`X509_get_signature_nid()` channel-binding path because BoringSSL does not
provide `X509_get_signature_info()`. The libpq patch also uses BoringSSL's
native protocol-version functions, guards OpenSSL-only error reason codes, and
provides explicit custom BIO lifecycle and control callbacks where BoringSSL
does not expose OpenSSL's BIO method getters. The Windows MSBuild properties
link BoringSSL's exported `ssl`/`crypto` libraries and their `d`-suffixed Debug
variants.

When updating the vcpkg baseline, refresh this directory from the matching
upstream `ports/libpq` directory, reapply the dependency change, and verify a
real `sslmode=verify-full` connection in addition to the normal build tests.
