#include "session.h"
#include "name.h"
#include "error.h"

static const char *const session_names[ORM_SQL_SESSION_VARIABLE_COUNT] = {
  "autocommit", "transaction_isolation", "transaction_read_only"
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
bool orm_sql_session_find(vstr name, orm_sql_session_variable *out) {
  if (!name.data || !out) return false;
  for (int i=0;i<ORM_SQL_SESSION_VARIABLE_COUNT;++i) {
    if (session_word(name,orm_sql_session_name((orm_sql_session_variable)i))) {
      *out=(orm_sql_session_variable)i; return true;
    }
  }
  return false;
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
  if (variable == ORM_SQL_SESSION_TRANSACTION_ISOLATION) *out = turbodb_text("SERIALIZABLE");
  else {
    const bool value = variable == ORM_SQL_SESSION_AUTOCOMMIT ? snapshot.autocommit : snapshot.read_only;
    *out = display ? turbodb_text(value ? "ON" : "OFF") : turbodb_i64(value ? 1 : 0);
  }
  return TURBODB_STATUS_OK;
}
