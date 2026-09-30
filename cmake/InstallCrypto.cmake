# Keep the upstream target definitions and matching binaries together so the
# installed static MySQL client never depends on a consumer's SSL toolchain.
string(TOLOWER "${CMAKE_BUILD_TYPE}" _crypto_configuration)
set(_crypto_binary_prefix "")
if(_crypto_configuration STREQUAL "debug")
  set(_crypto_binary_prefix "debug/")
endif()
install(FILES
  "${OpenSSL_DIR}/OpenSSLConfig.cmake"
  "${OpenSSL_DIR}/OpenSSLTargets.cmake"
  "${OpenSSL_DIR}/OpenSSLTargets-${_crypto_configuration}.cmake"
  DESTINATION share/OpenSSL)
install(DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/openssl"
  DESTINATION include)
install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/boringssl/copyright"
  DESTINATION share/boringssl)
foreach(_crypto_target IN ITEMS OpenSSL::Crypto OpenSSL::SSL)
  if(WIN32)
    install(FILES "$<TARGET_FILE:${_crypto_target}>" DESTINATION ${_crypto_binary_prefix}bin)
    install(FILES "$<TARGET_LINKER_FILE:${_crypto_target}>" DESTINATION ${_crypto_binary_prefix}lib)
  else()
    install(FILES "$<TARGET_FILE:${_crypto_target}>" DESTINATION ${_crypto_binary_prefix}lib)
  endif()
endforeach()
unset(_crypto_target)
unset(_crypto_configuration)
unset(_crypto_binary_prefix)
