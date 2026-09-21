#include <orm_driver_abi.h>
#include <cstddef>
#include <type_traits>

#define LAYOUT(T) static_assert(std::is_standard_layout<T>::value, #T " layout"); \
  static_assert(std::is_trivially_copyable<T>::value, #T " copy"); \
  static_assert(offsetof(T, header) == 0u, #T " prefix")
LAYOUT(orm_driver_api_v1);
LAYOUT(orm_driver_storage_capabilities_v1);
LAYOUT(orm_driver_host_v1);
LAYOUT(orm_driver_connection_v1);
LAYOUT(orm_driver_transaction_v1);
LAYOUT(orm_driver_cursor_v1);
LAYOUT(orm_driver_plan_view_v1);
LAYOUT(orm_driver_plan_meta_v1);
LAYOUT(orm_driver_limits_v1);
LAYOUT(orm_driver_value_v1);
using api_check = orm_status_t (ORM_DRIVER_CALL *)(const void *, uint32_t,
    const uint8_t *, orm_driver_bytes_v1, uint32_t, orm_error_t *);
static_assert(std::is_same<decltype(&orm_driver_validate_api_v1), api_check>::value,
              "validator signature and calling convention");
static_assert(ORM_C_ABI_VERSION == 4u, "candidate SDK must not change facade ABI");
int main() { return sizeof(orm_driver_header_v1) == 8u ? 0 : 1; }
