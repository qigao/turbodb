#include <orm_postgresql.h>

int main(void) {
  orm_error_t error;
  orm_error_init_s(&error, (uint32_t)sizeof(error));
  return orm_postgresql_register(&error) == ORM_STATUS_OK ? 0 : 1;
}
