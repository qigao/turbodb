#include <session_script.h>
#include <cmeta_error.h>
#define TINYMOCK_GENERATE_FUNCTION_OVERRIDES 1
#include <tinymock.h>

FunctionDeclResult(value, int, CMETA_RESULT_VALUE, script_execute,
    (const mysql_session_config_t *, config,
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER),
    (const uint8_t *, sql, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER),
    (size_t, size, CMETA_PARAM_IN,
     &cmeta_type_size, CMETA_ABI_SCALAR),
    (size_t, limit, CMETA_PARAM_IN,
     &cmeta_type_size, CMETA_ABI_SCALAR),
    (uint64_t *, count, CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER),
    (mysql_session_error_t *, error,
     CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER));
TINYMOCk_FUNCTION_DECLARE(script_execute);

static mysql_session_config_t captured_config;
static size_t captured_size;
static mysql_session_error_t injected_error;
static mysql_session_status_t test_execute(const mysql_session_config_t *config,
    const uint8_t *sql, size_t size, size_t limit, uint64_t *count,
    mysql_session_error_t *error) {
  captured_config = *config;
  captured_size = size;
  *count = 3u;
  *error = injected_error;
  return (mysql_session_status_t)script_execute(config, sql, size, limit, count, error);
}
#define mysql_session_execute_script test_execute
#include "../../../drivers/mysql/schema.c"
#undef mysql_session_execute_script

static const char connection_json[] =
    "{\"host\":\"test.invalid\",\"username\":\"unit\",\"password\":\"secret\","
    "\"database\":\"unit\",\"ca_file\":\"ca.pem\",\"server_name\":\"db.test\"}";

spec("MySQL dbtools schema adapter") {
  (void)ttest_config__;
  static void *context;
  static dbtool_error error;
  static dbtool_apply_result result;
  static const dbtool_schema_driver_ops *ops;
  before_each() {
    int returned = MYSQL_SESSION_OK;
    context = NULL;
    ops = dbtool_mysql_schema_driver();
    memset(&error, 0, sizeof(error));
    memset(&result, 0, sizeof(result));
    memset(&injected_error, 0, sizeof(injected_error));
    memset(&captured_config, 0, sizeof(captured_config));
    captured_size = 0u;
    TINYMOCk_FUNCTION_RESET(script_execute);
    check_true(TINYMOCk_FUNCTION_SET_RETURN(script_execute, returned));
  }
  after_each() {
    ops->close(context);
    TINYMOCk_FUNCTION_DESTROY(script_execute);
  }

  it("owns connection strings and forwards the complete script with verified TLS settings") {
    char json[sizeof(connection_json)];
    memcpy(json, connection_json, sizeof(json));
    dbtool_connection_config config = {NULL, json, 0u};
    const char sql[] = "create table alpha(id int); insert into alpha values(1); drop table alpha;";
    check_equal(ops->open(&context, &config, &error), DBTOOL_STATUS_OK);
    memset(json, 'x', sizeof(json));
    check_equal(ops->apply(context, sql, sizeof(sql) - 1u, &result, &error), DBTOOL_STATUS_OK);
    check_equal(captured_config.host, "test.invalid");
    check_equal(captured_config.password, "secret");
    check_equal(captured_config.ca_file, "ca.pem");
    check_equal(captured_config.server_name, "db.test");
    check_equal(captured_config.port, (uint16_t)3306u);
    check_equal(captured_config.timeout_ms, (uint32_t)5000u);
    check_equal(captured_size, sizeof(sql) - 1u);
    check_equal(result.statements, (uint64_t)3u);
    TINYMOCk_FUNCTION_VERIFY_TIMES(script_execute, 1u);
  }

  it("rejects missing TLS, malformed JSON and embedded NUL configuration without transport") {
    const char *invalid[] = {"{}", "[]", "{\"password\":\"secret\"",
        "{\"host\":\"test\\u0000invalid\"}", "{\"host\":1}"};
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      dbtool_connection_config config = {NULL, invalid[i], 0u};
      check_equal(ops->open(&context, &config, &error), DBTOOL_STATUS_INVALID_ARGUMENT);
      check_null(context);
      check_null(strstr(error.message, "secret"));
    }
    TINYMOCk_FUNCTION_VERIFY_NEVER(script_execute);
  }

  it("rejects unknown fields and invalid numeric ranges") {
    const char *suffixes[] = {",\"port\":0}", ",\"port\":65536}",
        ",\"timeout_ms\":4294967296}", ",\"timeout_ms\":1.5}", ",\"typo\":1}"};
    for (size_t i = 0u; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
      char json[sizeof(connection_json) + 64u];
      (void)snprintf(json, sizeof(json), "%.*s%s", (int)sizeof(connection_json) - 2,
                     connection_json, suffixes[i]);
      dbtool_connection_config config = {NULL, json, 0u};
      check_equal(ops->open(&context, &config, &error), DBTOOL_STATUS_INVALID_ARGUMENT);
      check_null(context);
    }
    TINYMOCk_FUNCTION_VERIFY_NEVER(script_execute);
  }

  it("maps SQL, authentication and unsupported results without retaining partial success") {
    dbtool_connection_config config = {NULL, connection_json, 0u};
    check_equal(ops->open(&context, &config, &error), DBTOOL_STATUS_OK);
    int returned = MYSQL_SESSION_SQL_ERROR;
    check_true(TINYMOCk_FUNCTION_SET_RETURN(script_execute, returned));
    injected_error.server_error = 1064u;
    check_equal(ops->apply(context, "broken", 6u, &result, &error), DBTOOL_STATUS_SQL_ERROR);
    check_equal(error.native_code, 1064);
    check_equal(result.statements, (uint64_t)0u);
    returned = MYSQL_SESSION_AUTH;
    check_true(TINYMOCk_FUNCTION_SET_RETURN(script_execute, returned));
    check_equal(ops->apply(context, "broken", 6u, &result, &error), DBTOOL_STATUS_CONNECTION_ERROR);
    returned = MYSQL_SESSION_UNSUPPORTED;
    check_true(TINYMOCk_FUNCTION_SET_RETURN(script_execute, returned));
    check_equal(ops->apply(context, "select 1", 8u, &result, &error), DBTOOL_STATUS_UNSUPPORTED);
    TINYMOCk_FUNCTION_VERIFY_TIMES(script_execute, 3u);
  }

  it("rejects oversized and embedded NUL scripts before executing") {
    dbtool_connection_config config = {NULL, connection_json, 0u};
    const char sql[] = {'a',0,'b'};
    check_equal(ops->open(&context, &config, &error), DBTOOL_STATUS_OK);
    check_equal(ops->apply(context, sql, sizeof(sql), &result, &error), DBTOOL_STATUS_INVALID_ARGUMENT);
    check_equal(ops->apply(context, sql, SIZE_MAX, &result, &error), DBTOOL_STATUS_LIMIT_EXCEEDED);
    TINYMOCk_FUNCTION_VERIFY_NEVER(script_execute);
  }
}
