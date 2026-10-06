#include "name.h"

static bool name_start(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
turbodb_status_t orm_sql_name_validate(vstr name, const char **reason) {
  if (name.len > ORM_SQL_SELECT_NAME_BYTES) {
    *reason = "identifier exceeds name capacity"; return TURBODB_STATUS_LIMIT_EXCEEDED;
  }
  if (!name.data || !name.len || !name_start((unsigned char)name.data[0])) {
    *reason = "expected an ASCII identifier"; return TURBODB_STATUS_UNSUPPORTED;
  }
  for (size_t i = 1; i < name.len; ++i)
    if (!name_start((unsigned char)name.data[i]) && (name.data[i] < '0' || name.data[i] > '9')) {
      *reason = "expected an ASCII identifier"; return TURBODB_STATUS_UNSUPPORTED;
    }
  return TURBODB_STATUS_OK;
}
void orm_sql_name_space(vstr *text) {
  while (text->len && (text->data[0] == ' ' || text->data[0] == '\t' ||
      text->data[0] == '\n' || text->data[0] == '\r' || text->data[0] == '\f')) {
    ++text->data; --text->len;
  }
}
turbodb_status_t orm_sql_name_node(const sqlparser_document *document, sqlparser_id id,
    vstr *out, const char **reason) {
  const sqlparser_node *node = sqlparser_get_node(document, id);
  if (!node || node->kind != SQLPARSER_NAME || node->as.name.parts != 1) {
    *reason = "expected an unqualified identifier"; return TURBODB_STATUS_UNSUPPORTED;
  }
  vstr text = {sqlparser_text(document, node->span), node->span.length}, name = {0};
  const turbodb_status_t status = orm_sql_name_part(&text, &name, reason);
  if (status != TURBODB_STATUS_OK) return status;
  if (text.len) { *reason = "unsupported identifier quoting"; return TURBODB_STATUS_UNSUPPORTED; }
  *out = name; return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_name_part(vstr *text, vstr *out, const char **reason) {
  orm_sql_name_space(text);
  const bool quoted = text->len && text->data[0] == '`';
  if (quoted) { ++text->data; --text->len; }
  size_t count = 0;
  while (count < text->len && (name_start((unsigned char)text->data[count]) ||
      (text->data[count] >= '0' && text->data[count] <= '9'))) {
    if (count == ORM_SQL_SELECT_NAME_BYTES) {
      *reason = "identifier exceeds name capacity"; return TURBODB_STATUS_LIMIT_EXCEEDED;
    }
    ++count;
  }
  *out = (vstr){text->data, count};
  turbodb_status_t status = orm_sql_name_validate(*out, reason);
  if (status != TURBODB_STATUS_OK) return status;
  text->data += count; text->len -= count;
  if (quoted) {
    if (!text->len || text->data[0] != '`') {
      *reason = "unsupported quoted identifier"; return TURBODB_STATUS_UNSUPPORTED;
    }
    ++text->data; --text->len;
  }
  orm_sql_name_space(text);
  return TURBODB_STATUS_OK;
}
