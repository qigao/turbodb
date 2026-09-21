#include <orm.hpp>

#include <type_traits>

using retain_connection_fn = orm_status_t (ORM_C_CALL *)(orm_connection_t *);
using close_connection_fn =
    orm_status_t (ORM_C_CALL *)(orm_connection_t *, orm_error_t *);

static_assert(std::is_same_v<decltype(&orm_connection_retain), retain_connection_fn>);
static_assert(std::is_same_v<decltype(&orm_connection_close), close_connection_fn>);
static_assert(std::is_same_v<decltype(&orm::connection::close),
                             void (orm::connection::*)()>);
static_assert(std::is_same_v<decltype(&orm::query::close),
                             void (orm::query::*)()>);
static_assert(std::is_same_v<decltype(&orm::transaction::close),
                             void (orm::transaction::*)()>);

int main() {
  retain_connection_fn volatile retain = &orm_connection_retain;
  close_connection_fn volatile close = &orm_connection_close;
  return retain(nullptr) == ORM_STATUS_INVALID_ARGUMENT &&
                 close(nullptr, nullptr) == ORM_STATUS_INVALID_ARGUMENT
             ? 0
             : 1;
}
