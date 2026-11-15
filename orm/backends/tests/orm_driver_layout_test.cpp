#include <orm_driver_base.h>

#include <cstddef>
#include <type_traits>

static_assert(sizeof(orm_driver_header_v1) == 8u, "driver header size");
static_assert(offsetof(orm_driver_header_v1, struct_size) == 0u, "size offset");
static_assert(offsetof(orm_driver_header_v1, abi_version) == 4u, "version offset");
static_assert(std::is_standard_layout<orm_driver_header_v1>::value, "header layout");
static_assert(std::is_standard_layout<orm_driver_bytes_v1>::value, "view layout");
static_assert(std::is_standard_layout<orm_driver_table_v1>::value, "table layout");
static_assert(std::is_same<decltype(orm_driver_bytes_v1::size), uint64_t>::value,
              "view length width");

int main() { return 0; }
