#include <orm.h>

#include <stdio.h>

int main(void) {
  const orm_option_t option = {orm_view("filename"), orm_view(":memory:")};
  orm_config_t config;
  orm_connection_t *connection = 0;
  orm_error_t error;

  orm_config(&config);
  orm_error_init(&error);
  config.driver = orm_view("sqlite");
  config.options = &option;
  config.option_count = 1;
  if (orm_connect(&config, &connection, &error) != ORM_STATUS_OK) {
    fprintf(stderr, "core consumer connect failed: %s\n", error.message);
    return 1;
  }
  orm_disconnect(connection);
  return 0;
}
