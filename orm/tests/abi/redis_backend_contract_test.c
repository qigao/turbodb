#include "orm.h"

#include <stdio.h>
#include <string.h>

static orm_string_view_t view(const char *text) {
  return orm_view(text);
}

int main(void) {
  orm_config_t config;
  orm_error_t error;
  orm_connection_t *connection = NULL;
  orm_option_t invalid_option = {view("scan_fallback"), view("true")};

  orm_config(&config);
  orm_error_init(&error);
  config.driver = view("redis");
  config.options = &invalid_option;
  config.option_count = 1;
  if (orm_connect(&config, &connection, &error) != ORM_STATUS_INVALID_ARGUMENT ||
      connection != NULL) {
    fprintf(stderr, "Redis ORM accepted an unknown/fallback option: %s\n", error.message);
    return 1;
  }

  invalid_option.keyword = view("port");
  invalid_option.value = view("1");
  config.options = &invalid_option;
  config.option_count = 1;
  orm_error_init(&error);
  if (orm_connect(&config, &connection, &error) != ORM_STATUS_CONNECTION_ERROR ||
      connection != NULL || strstr(error.message, "CFlow") == NULL) {
    fprintf(stderr, "Redis ORM did not report its CFlow connect failure: %s\n",
            error.message);
    orm_disconnect(connection);
    return 1;
  }
  return 0;
}
