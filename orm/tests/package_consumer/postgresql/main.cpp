#include <orm_postgresql.hpp>

#include <type_traits>
#include <utility>

static_assert(
    std::is_same_v<decltype(orm::postgresql_connection(std::declval<const orm::config &>())),
                   orm::connection>);

int main() { return 0; }
