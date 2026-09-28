#include "session.h"

#include <stdio.h>
#include <stdlib.h>

static const char *required_env(const char *name) {
  const char *value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    fprintf(stderr, "missing required environment: %s\n", name);
    return NULL;
  }
  return value;
}

int main(void) {
  const char *host = required_env("ORM_MYSQL_HOST");
  const char *port_text = required_env("ORM_MYSQL_PORT");
  const char *user = required_env("ORM_MYSQL_USER");
  const char *password = required_env("ORM_MYSQL_PASSWORD");
  const char *ca_file = required_env("ORM_MYSQL_CA_FILE");
  const char *server_name = required_env("ORM_MYSQL_SERVER_NAME");
  mysql_session_config_t config;
  mysql_session_error_t error;
  unsigned long port;
  mysql_session_status_t status;

  if (host == NULL || port_text == NULL || user == NULL || password == NULL ||
      ca_file == NULL || server_name == NULL)
    return 2;

  port = strtoul(port_text, NULL, 10);
  if (port == 0ul || port > 65535ul) {
    fprintf(stderr, "invalid ORM_MYSQL_PORT: %s\n", port_text);
    return 2;
  }

  config = (mysql_session_config_t){
      .host = host,
      .port = (uint16_t)port,
      .username = user,
      .password = password,
      .database = getenv("ORM_MYSQL_DATABASE"),
      .ca_file = ca_file,
      .server_name = server_name,
      .timeout_ms = 10000u};

  status = mysql_session_connect_and_ping(&config, &error);
  if (status != MYSQL_SESSION_OK) {
    fprintf(stderr,
            "mysql session failed status=%d stage=%s cnet=%d native=%d "
            "server=%u sqlstate=%s message=%s\n",
            (int)status, error.stage, error.cnet_status,
            error.cnet_native_status, (unsigned int)error.server_error,
            error.sql_state, error.message);
    return 1;
  }

  return 0;
}
