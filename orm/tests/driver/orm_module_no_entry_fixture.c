#include <stdint.h>

#if defined(_WIN32)
#define TEST_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define TEST_EXPORT __attribute__((visibility("default")))
#else
#define TEST_EXPORT
#endif

TEST_EXPORT int orm_module_no_entry_fixture_marker(void) {
  return 1;
}
