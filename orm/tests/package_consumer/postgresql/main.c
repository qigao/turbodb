#include <orm_postgresql.h>

int main(void) {
  orm_status_t(ORM_C_CALL * connector)(const orm_config_t *, orm_connection_t **, orm_error_t *) =
      &orm_postgresql_connect;
  return connector == NULL;
}
