#include "orm_postgresql.h"

#include "orm_c_internal.hpp"

extern "C" orm_status_t ORM_C_CALL
orm_postgresql_register(orm_error_t* error)
{
    return orm_c_detail::register_database_driver(
        "postgresql", &orm_c_detail::make_postgres_backend, error);
}
