#include "session.h"
#include "name.h"
#include "error.h"

static const char *const session_names[ORM_SQL_SESSION_VARIABLE_COUNT] = {
  "autocommit", "transaction_isolation", "transaction_read_only",
  "auto_increment_increment", "character_set_client", "character_set_connection",
  "character_set_results", "character_set_server", "collation_server",
  "collation_connection", "init_connect", "interactive_timeout", "license",
  "lower_case_table_names", "max_allowed_packet", "net_write_timeout",
  "performance_schema", "query_cache_size", "query_cache_type", "sql_mode",
  "system_time_zone", "time_zone", "wait_timeout"
};
static bool session_word(vstr text, vstr expected) {
  if (text.len != expected.len) return false;
  for (size_t i = 0; i < text.len; ++i) {
    const char ch = text.data[i];
    if ((ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch) != expected.data[i]) return false;
  }
  return true;
}
static turbodb_status_t session_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  tdsql_error_set(error, status, reason); return status;
}
vstr orm_sql_session_name(orm_sql_session_variable variable) {
  return variable >= 0 && variable < ORM_SQL_SESSION_VARIABLE_COUNT ?
      vstr_from_cstr(session_names[variable]) : (vstr){0};
}
orm_sql_type orm_sql_session_type(orm_sql_session_variable variable) {
  const bool text=variable==ORM_SQL_SESSION_TRANSACTION_ISOLATION ||
      (variable>=ORM_SQL_SESSION_CHARACTER_SET_CLIENT &&
       variable<=ORM_SQL_SESSION_INIT_CONNECT) || variable==ORM_SQL_SESSION_LICENSE ||
      variable==ORM_SQL_SESSION_QUERY_CACHE_TYPE || variable==ORM_SQL_SESSION_SQL_MODE ||
      variable==ORM_SQL_SESSION_SYSTEM_TIME_ZONE || variable==ORM_SQL_SESSION_TIME_ZONE;
  const turbodb_value_kind_t kind=text?TURBODB_VALUE_TEXT:
      variable<ORM_SQL_SESSION_WRITABLE_COUNT?TURBODB_VALUE_INT64:TURBODB_VALUE_UINT64;
  return (orm_sql_type){kind,true};
}
bool orm_sql_session_find(vstr name, orm_sql_session_variable *out) {
  if (!name.data || !out) return false;
  if (session_word(name,vstr_from_cstr("tx_isolation"))) {
    *out=ORM_SQL_SESSION_TRANSACTION_ISOLATION; return true;
  }
  for (int i=0;i<ORM_SQL_SESSION_VARIABLE_COUNT;++i) {
    if (session_word(name,orm_sql_session_name((orm_sql_session_variable)i))) {
      *out=(orm_sql_session_variable)i; return true;
    }
  }
  return false;
}
bool orm_sql_session_find_writable(vstr name, orm_sql_session_variable *out) {
  orm_sql_session_variable variable;
  if (!out || !orm_sql_session_find(name,&variable) || variable>=ORM_SQL_SESSION_WRITABLE_COUNT)
    return false;
  *out=variable; return true;
}
turbodb_status_t orm_sql_session_resolve(const sqlparser_document *document,
    const sqlparser_node *node, orm_tidesdb_sql_budget *budget,
    orm_sql_session_variable *out, turbodb_error_t *error) {
  if (!document || !node || node->kind != SQLPARSER_VARIABLE || !budget || !out)
    return session_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL session variable");
  vstr text = {sqlparser_text(document,node->span), node->span.length}, name = {0};
  if (!text.data) return session_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL variable span");
  orm_sql_budget_amount amount = {0};
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = text.len;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(budget,&amount,error);
  if (status != TURBODB_STATUS_OK) return status;
  enum { SYSTEM_VARIABLE_PREFIX_BYTES = 2 };
  if (text.len <= SYSTEM_VARIABLE_PREFIX_BYTES || text.data[0] != '@' || text.data[1] != '@')
    return session_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL user variables are unsupported");
  text.data += SYSTEM_VARIABLE_PREFIX_BYTES; text.len -= SYSTEM_VARIABLE_PREFIX_BYTES;
  const char *reason = NULL;
  status = orm_sql_name_part(&text,&name,&reason);
  if (status != TURBODB_STATUS_OK) return session_error(error,status,reason);
  if (text.len && text.data[0] == '.') {
    if (!session_word(name,vstr_from_cstr("session")) && !session_word(name,vstr_from_cstr("local")))
      return session_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL variables require SESSION or LOCAL scope");
    ++text.data; --text.len;
    status = orm_sql_name_part(&text,&name,&reason);
    if (status != TURBODB_STATUS_OK) return session_error(error,status,reason);
  }
  if (text.len) return session_error(error, TURBODB_STATUS_UNSUPPORTED, "unsupported SQL variable identifier");
  if (orm_sql_session_find(name,out)) return TURBODB_STATUS_OK;
  return session_error(error, TURBODB_STATUS_UNSUPPORTED, "unsupported SQL session variable name");
}
turbodb_status_t orm_sql_session_value(orm_sql_session_snapshot snapshot,
    orm_sql_session_variable variable, bool display, turbodb_value_t *out, turbodb_error_t *error) {
  if (!out || variable < 0 || variable >= ORM_SQL_SESSION_VARIABLE_COUNT)
    return session_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL session variable output");
  if (!snapshot.valid)
    return session_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL session variable requires connection context");
  switch(variable) {
    case ORM_SQL_SESSION_AUTOCOMMIT: case ORM_SQL_SESSION_TRANSACTION_READ_ONLY: {
      const bool value=variable==ORM_SQL_SESSION_AUTOCOMMIT?snapshot.autocommit:snapshot.read_only;
      *out=display?turbodb_text(value?"ON":"OFF"):turbodb_i64(value?1:0); break;
    }
    case ORM_SQL_SESSION_TRANSACTION_ISOLATION: *out=turbodb_text("SERIALIZABLE"); break;
    case ORM_SQL_SESSION_AUTO_INCREMENT_INCREMENT: *out=turbodb_u64(1); break;
    case ORM_SQL_SESSION_CHARACTER_SET_CLIENT:
    case ORM_SQL_SESSION_CHARACTER_SET_CONNECTION:
    case ORM_SQL_SESSION_CHARACTER_SET_SERVER: *out=turbodb_text("utf8mb4"); break;
    case ORM_SQL_SESSION_CHARACTER_SET_RESULTS:
    case ORM_SQL_SESSION_INTERACTIVE_TIMEOUT:
    case ORM_SQL_SESSION_NET_WRITE_TIMEOUT:
    case ORM_SQL_SESSION_WAIT_TIMEOUT: *out=turbodb_null(); break;
    case ORM_SQL_SESSION_COLLATION_SERVER:
    case ORM_SQL_SESSION_COLLATION_CONNECTION: *out=turbodb_text("utf8mb4_bin"); break;
    case ORM_SQL_SESSION_INIT_CONNECT: *out=turbodb_text(""); break;
    case ORM_SQL_SESSION_LICENSE: *out=turbodb_text("Apache-2.0"); break;
    case ORM_SQL_SESSION_LOWER_CASE_TABLE_NAMES: *out=turbodb_u64(0); break;
    case ORM_SQL_SESSION_MAX_ALLOWED_PACKET: *out=turbodb_u64(snapshot.max_allowed_packet); break;
    case ORM_SQL_SESSION_PERFORMANCE_SCHEMA:
      *out=display?turbodb_text("OFF"):turbodb_u64(0); break;
    case ORM_SQL_SESSION_QUERY_CACHE_SIZE: *out=turbodb_u64(0); break;
    case ORM_SQL_SESSION_QUERY_CACHE_TYPE: *out=turbodb_text("OFF"); break;
    case ORM_SQL_SESSION_SQL_MODE:
      *out=turbodb_text("NO_BACKSLASH_ESCAPES,STRICT_TRANS_TABLES"); break;
    case ORM_SQL_SESSION_SYSTEM_TIME_ZONE: *out=turbodb_text("UTC"); break;
    case ORM_SQL_SESSION_TIME_ZONE: *out=turbodb_text("+00:00"); break;
    default: return session_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL session variable output");
  }
  return TURBODB_STATUS_OK;
}
