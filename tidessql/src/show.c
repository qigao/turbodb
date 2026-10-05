#include "show.h"
#include "value.h"
#include "name.h"
#include "store_internal.h"
#include "work.h"
#include "error.h"
#include <sds.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { SHOW_FIELD, SHOW_TYPE, SHOW_NULL, SHOW_KEY, SHOW_DEFAULT, SHOW_EXTRA };
enum { SHOW_INDEX_TABLE, SHOW_INDEX_NON_UNIQUE, SHOW_INDEX_NAME, SHOW_INDEX_SEQUENCE,
       SHOW_INDEX_COLUMN, SHOW_INDEX_COLLATION, SHOW_INDEX_CARDINALITY, SHOW_INDEX_SUB_PART,
       SHOW_INDEX_PACKED, SHOW_INDEX_NULL, SHOW_INDEX_TYPE, SHOW_INDEX_COMMENT,
       SHOW_INDEX_INDEX_COMMENT, SHOW_INDEX_VISIBLE, SHOW_INDEX_EXPRESSION };
enum { SHOW_KEY_NONE, SHOW_KEY_MULTIPLE, SHOW_KEY_UNIQUE, SHOW_KEY_PRIMARY };
static const char *const show_keys[] = {"", "MUL", "UNI", "PRI"};
static const char show_prefix[] = "Tables_in_";
enum { SHOW_PATTERN_DEPTH = 1, SHOW_VARIABLE_COLUMNS = 2 };
static turbodb_status_t show_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  tdsql_error_set(error, status, reason); return status;
}
static turbodb_status_t show_pattern_open(const sqlparser_document *document,
    sqlparser_id id, orm_sql_show_source *source, turbodb_error_t *error) {
  if (!id) return TURBODB_STATUS_OK;
  const sqlparser_node *node=sqlparser_get_node(document,id);
  if (!node || node->kind!=SQLPARSER_STRING)
    return show_error(error,TURBODB_STATUS_UNSUPPORTED,"SQL SHOW LIKE requires a string literal");
  turbodb_status_t status=orm_tidesdb_sql_expr_compile_value(document,id,NULL,0,SHOW_PATTERN_DEPTH,
      source->source.budget,&source->pattern,error);
  if (status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_expr_eval(&source->pattern,
      NULL,0,&source->pattern_value,error);
  const orm_sql_type text={TURBODB_VALUE_TEXT,false};
  const int escape=sqlparser_get_options(document).mysql_no_backslash_escapes ? ORM_SQL_LIKE_NO_ESCAPE : '\\';
  if (status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_like_bind(text,text,escape,false,&source->like,error);
  source->like.ascii_insensitive=source->kind!=SQLPARSER_SHOW_TABLES;
  /* Validate even when the pattern matches no variable. */
  const turbodb_value_t empty=turbodb_text(""); turbodb_value_t ignored=turbodb_null();
  if (status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_like_eval(&source->like,
      &empty,&source->pattern_value,source->source.budget,&ignored,error);
  return status;
}
static turbodb_status_t show_limit_value(const sqlparser_document *document,
    sqlparser_id id, uint64_t *out, turbodb_error_t *error) {
  const sqlparser_node *node=sqlparser_get_node(document,id);
  if (!node || node->kind!=SQLPARSER_NUMBER)
    return show_error(error,TURBODB_STATUS_UNSUPPORTED,
        "SQL SHOW WARNINGS LIMIT requires a nonnegative integer literal");
  turbodb_value_t value=turbodb_null(); turbodb_error_t cause; tdsql_error_init(&cause);
  const vstr text={sqlparser_text(document,node->span),node->span.length};
  const turbodb_status_t status=orm_tidesdb_sql_integer_literal(text,false,&value,&cause);
  if (status!=TURBODB_STATUS_OK) return show_error(error,status,cause.message);
  *out=value.kind==TURBODB_VALUE_UINT64?value.data.uint64_value:
      (uint64_t)value.data.int64_value;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t show_limit_open(const sqlparser_document *document,
    sqlparser_id id, orm_sql_show_source *source, turbodb_error_t *error) {
  source->limit=UINT64_MAX;
  if (!id) return TURBODB_STATUS_OK;
  const sqlparser_node *node=sqlparser_get_node(document,id);
  if (!node || node->kind!=SQLPARSER_LIMIT)
    return show_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
        "SQL SHOW WARNINGS has an invalid LIMIT");
  turbodb_status_t status=show_limit_value(document,node->as.limit.count,
      &source->limit,error);
  if (status==TURBODB_STATUS_OK && node->as.limit.offset)
    status=show_limit_value(document,node->as.limit.offset,&source->offset,error);
  return status;
}
static turbodb_status_t show_table(const sqlparser_document *document, sqlparser_id id, vstr *out, turbodb_error_t *error) {
  const char *reason = NULL;
  const turbodb_status_t status = orm_sql_name_node(document, id, out, &reason);
  return status == TURBODB_STATUS_OK ? status : show_error(error, status, reason);
}
static const char *show_column_type(turbodb_value_kind_t kind) {
  switch (kind) {
    case TURBODB_VALUE_INT64: return "bigint";
    case TURBODB_VALUE_UINT64: return "bigint unsigned";
    case TURBODB_VALUE_DOUBLE: return "double";
    default: return NULL;
  }
}
static vstr show_default_text(const orm_sql_column_default *column_default,
    char text[64]) {
  int length = 0;
  if (!column_default->specified || column_default->value.kind == TURBODB_VALUE_NULL)
    return (vstr){0};
  if (column_default->value.kind == TURBODB_VALUE_INT64)
    length = snprintf(text,64,"%" PRId64,column_default->value.data.int64_value);
  else if (column_default->value.kind == TURBODB_VALUE_UINT64)
    length = snprintf(text,64,"%" PRIu64,column_default->value.data.uint64_value);
  else if (column_default->value.kind == TURBODB_VALUE_DOUBLE)
    length = snprintf(text,64,"%.17g",column_default->value.data.double_value);
  return length > 0 && length < 64 ? (vstr){text,(size_t)length} : (vstr){0};
}
typedef struct show_create_writer {
  tstr text;
  size_t length;
  orm_tidesdb_sql_budget *budget;
  turbodb_error_t *error;
  turbodb_status_t status;
} show_create_writer;
/* Both passes visit identical fragments; the second pass cannot grow tstr. */
static void show_create_piece(show_create_writer *writer, vstr text) {
  if (writer->status != TURBODB_STATUS_OK) return;
  if (text.len > SIZE_MAX - writer->length) {
    writer->status = show_error(writer->error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL SHOW CREATE text length overflow"); return;
  }
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = text.len;
  writer->status = orm_tidesdb_sql_budget_reserve(writer->budget, &amount, writer->error);
  if (writer->status != TURBODB_STATUS_OK) return;
  if (writer->text) {
    if (text.len > tstr_avail(writer->text)) {
      writer->status = show_error(writer->error, TURBODB_STATUS_INTERNAL_ERROR, "SQL SHOW CREATE measured capacity mismatch"); return;
    }
    tstr next = tstr_cat_len(writer->text, text.data, text.len);
    if (!next) { writer->status = show_error(writer->error, TURBODB_STATUS_OUT_OF_MEMORY, "SQL SHOW CREATE append failed"); return; }
    writer->text = next;
  }
  writer->length += text.len;
}
static void show_create_literal(show_create_writer *writer, const char *text) {
  show_create_piece(writer, vstr_from_cstr(text));
}
static void show_create_name(show_create_writer *writer, vstr name) {
  static const char quote[] = "`";
  show_create_literal(writer, quote); show_create_piece(writer, name); show_create_literal(writer, quote);
}
static void show_create_render(show_create_writer *writer, const orm_sql_show_source *source,
    const orm_sql_table_schema *table) {
  static const char separator[] = ",\n  ";
  show_create_literal(writer, "CREATE TABLE "); show_create_name(writer, table->name);
  show_create_literal(writer, " (\n  ");
  for (size_t i = 0; writer->status == TURBODB_STATUS_OK && i < table->count; ++i) {
    const orm_sql_schema_column *column = &table->columns[i];
    const orm_sql_column_default *column_default = &table->defaults[i];
    const char *type = show_column_type(column->type.kind);
    if (!type) { writer->status = show_error(writer->error, TURBODB_STATUS_DATASTORE_ERROR, "SQL SHOW invalid persisted column type"); break; }
    if (i) show_create_literal(writer, separator);
    show_create_name(writer, column->name); show_create_literal(writer, " ");
    show_create_literal(writer, type); show_create_literal(writer, column->type.nullable ? " NULL" : " NOT NULL");
    if (column_default->specified) {
      show_create_literal(writer," DEFAULT ");
      if (column_default->value.kind == TURBODB_VALUE_NULL)
        show_create_literal(writer,"NULL");
      else {
        char text[64];
        const vstr value = show_default_text(column_default,text);
        if (!value.data)
          writer->status = show_error(writer->error,TURBODB_STATUS_DATASTORE_ERROR,
              "SQL SHOW invalid persisted column default");
        else show_create_piece(writer,value);
      }
    }
  }
  show_create_literal(writer, separator); show_create_literal(writer, "PRIMARY KEY (");
  show_create_name(writer, table->columns[source->definition.primary_key].name); show_create_literal(writer, ")");
  for (size_t i = 0; writer->status == TURBODB_STATUS_OK && i < vec_size(&source->indexes.records); ++i) {
    const orm_sql_index_record *record = vec_at_const(&source->indexes.records, i);
    show_create_literal(writer, separator); show_create_literal(writer, record->definition.unique ? "UNIQUE KEY " : "KEY ");
    show_create_name(writer, (vstr){record->definition.name, record->definition.name_size}); show_create_literal(writer, " (");
    for (size_t j = 0; writer->status == TURBODB_STATUS_OK && j < vec_size(&record->definition.parts); ++j) {
      const orm_sql_index_part *part = vec_at_const(&record->definition.parts, j);
      if (j) show_create_literal(writer, ", ");
      show_create_name(writer, table->columns[part->column].name);
      show_create_literal(writer, part->descending ? " DESC" : " ASC");
    }
    show_create_literal(writer, ")");
  }
  show_create_literal(writer, "\n)");
}
static turbodb_status_t show_create_open(orm_sql_show_source *source, const orm_sql_table_schema *table, turbodb_error_t *error) {
  show_create_writer writer = {.budget = source->source.budget, .error = error};
  show_create_render(&writer, source, table);
  if (writer.status != TURBODB_STATUS_OK) return writer.status;
  /* tstr_new_len uses exact SDS capacity; account for the largest public SDS
   * header and terminator before allocating, without relying on header choice. */
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve_capacity(writer.budget, writer.length, 1,
      sizeof(struct sdshdr64) + 1, &source->create_bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  source->create_sql = tstr_new_len(NULL, writer.length);
  if (!source->create_sql) return show_error(error, TURBODB_STATUS_OUT_OF_MEMORY, "SQL SHOW CREATE allocation failed");
  if (!tstr_set_len_checked(source->create_sql, 0))
    return show_error(error, TURBODB_STATUS_INTERNAL_ERROR, "SQL SHOW CREATE length reset failed");
  writer.text = source->create_sql; writer.length = 0;
  show_create_render(&writer, source, table); source->create_sql = writer.text;
  return writer.status;
}
static turbodb_status_t show_keys_open(orm_sql_show_source *source, const orm_sql_table_schema *table, turbodb_error_t *error) {
  turbodb_status_t status = orm_sql_work_zero(&source->keys, table->count, sizeof(uint8_t),
      _Alignof(uint8_t), source->source.budget, &source->key_bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_budget_amount amount = {0};
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = table->count;
  status = orm_tidesdb_sql_budget_reserve(source->source.budget, &amount, error);
  if (status != TURBODB_STATUS_OK) return status;
  *(uint8_t *)vec_at(&source->keys, source->definition.primary_key) = SHOW_KEY_PRIMARY;
  for (size_t i = 0; i < vec_size(&source->indexes.records); ++i) {
    amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
    status = orm_tidesdb_sql_budget_reserve(source->source.budget, &amount, error);
    if (status != TURBODB_STATUS_OK) return status;
    const orm_sql_index_record *record = vec_at_const(&source->indexes.records, i);
    const orm_sql_index_part *first = vec_at_const(&record->definition.parts, 0);
    uint8_t *key = vec_at(&source->keys, first->column);
    const uint8_t rank = record->definition.unique && vec_size(&record->definition.parts) == 1
        ? SHOW_KEY_UNIQUE : SHOW_KEY_MULTIPLE;
    if (*key < rank) *key = rank;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t show_definition(const sqlparser_document *document, sqlparser_id id,
    orm_sql_catalog_store *store, orm_sql_show_source *source, turbodb_error_t *error) {
  vstr name = {0}; uint64_t table_id = 0, version = 0; bool found = false;
  orm_sql_catalog_snapshot snapshot = {0}; orm_sql_table_schema table = {0};
  turbodb_status_t status = show_table(document, id, &name, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_store_lookup(store, name, &source->definition,
      &table_id, &version, &found, &snapshot, error);
  if (status == TURBODB_STATUS_OK && !found) status = show_error(error, TURBODB_STATUS_SQL_ERROR, "SQL SHOW table does not exist");
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_catalog_schema(&source->definition, &table, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_index_set_load(store, &table, table_id, snapshot, &source->indexes, error);
  if (status == TURBODB_STATUS_OK && snapshot.format == ORM_SQL_STORE_FORMAT_BASE && vec_size(&source->indexes.records))
    status = show_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL SHOW index directory in base format");
  if (status == TURBODB_STATUS_OK && source->kind == SQLPARSER_SHOW_COLUMNS) status = show_keys_open(source, &table, error);
  if (status == TURBODB_STATUS_OK && source->kind == SQLPARSER_SHOW_CREATE_TABLE) status = show_create_open(source, &table, error);
  if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true;
  return status;
}
/* Definitions were checked against this schema during open. No I/O or allocation
 * while enumerating; the owner lease keeps the snapshot stable through close. */
static void show_index_read(orm_sql_show_source *source, const orm_sql_table_schema *table) {
  size_t column = source->definition.primary_key;
  vstr name = vstr_from_cstr("PRIMARY"); bool unique = true, descending = false;
  size_t parts = 1;
  if (source->position) {
    const orm_sql_index_record *record = vec_at_const(&source->indexes.records, source->position - 1);
    const orm_sql_index_part *part = vec_at_const(&record->definition.parts, source->part);
    column = part->column; descending = part->descending; unique = record->definition.unique;
    name = (vstr){record->definition.name, record->definition.name_size};
    parts = vec_size(&record->definition.parts);
  }
  source->values[SHOW_INDEX_TABLE] = turbodb_text_v(table->name);
  source->values[SHOW_INDEX_NON_UNIQUE] = turbodb_i64(unique ? 0 : 1);
  source->values[SHOW_INDEX_NAME] = turbodb_text_v(name);
  source->values[SHOW_INDEX_SEQUENCE] = turbodb_i64((int64_t)source->part + 1);
  source->values[SHOW_INDEX_COLUMN] = turbodb_text_v(table->columns[column].name);
  source->values[SHOW_INDEX_COLLATION] = turbodb_text(descending ? "D" : "A");
  source->values[SHOW_INDEX_CARDINALITY] = turbodb_null();
  source->values[SHOW_INDEX_SUB_PART] = turbodb_null();
  source->values[SHOW_INDEX_PACKED] = turbodb_null();
  source->values[SHOW_INDEX_NULL] = turbodb_text(table->columns[column].type.nullable ? "YES" : "");
  source->values[SHOW_INDEX_TYPE] = turbodb_text("LSM");
  source->values[SHOW_INDEX_COMMENT] = turbodb_text("");
  source->values[SHOW_INDEX_INDEX_COMMENT] = turbodb_text("");
  source->values[SHOW_INDEX_VISIBLE] = turbodb_text("YES");
  source->values[SHOW_INDEX_EXPRESSION] = turbodb_null();
  if (++source->part == parts) { source->part = 0; ++source->position; }
}
static turbodb_status_t show_read(orm_sql_show_source *source, const turbodb_value_t **out, turbodb_error_t *error) {
  turbodb_status_t status = orm_sql_store_ready(source->owner, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  status = orm_tidesdb_sql_budget_reserve(source->source.budget, &amount, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (source->kind==SQLPARSER_SHOW_VARIABLES) {
    if (source->position<ORM_SQL_SESSION_VARIABLE_COUNT) {
      const orm_sql_session_variable variable=(orm_sql_session_variable)source->position++;
      source->values[0]=turbodb_text_v(orm_sql_session_name(variable));
      status=orm_sql_session_value(source->session,variable,true,
          &source->values[1],error);
      if (status!=TURBODB_STATUS_OK) return status;
      *out=source->values; return TURBODB_STATUS_OK;
    }
    source->done=true; *out=NULL; return TURBODB_STATUS_OK;
  }
  if (source->kind==SQLPARSER_SHOW_WARNINGS) {
    if (source->count) {
      if (source->position) { source->done=true; *out=NULL; return TURBODB_STATUS_OK; }
      source->values[0]=turbodb_u64(source->diagnostics?source->diagnostics->total:0);
      ++source->position; *out=source->values; return TURBODB_STATUS_OK;
    }
    const size_t retained=source->diagnostics?
        vec_size(&source->diagnostics->records):0;
    if (source->position==retained) {
      source->done=true; *out=NULL; return TURBODB_STATUS_OK;
    }
    const orm_sql_diagnostic *record=orm_sql_diagnostics_at(
        source->diagnostics,source->position++);
    source->values[0]=turbodb_text("Warning");
    source->values[1]=turbodb_i64((int64_t)record->code);
    source->values[2]=turbodb_text(record->message);
    *out=source->values; return TURBODB_STATUS_OK;
  }
  orm_sql_table_schema table = {0};
  if (source->kind == SQLPARSER_SHOW_TABLES) {
    status = orm_tidesdb_sql_catalog_destroy(&source->definition, error);
    uint64_t id = 0, version = 0; bool found = false;
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_catalog_cursor_next(&source->cursor,
        &source->definition, &id, &version, &found, error);
    if (status != TURBODB_STATUS_OK) return status;
    if (!found) { source->done = true; *out = NULL; return TURBODB_STATUS_OK; }
  }
  status = orm_tidesdb_sql_catalog_schema(&source->definition, &table, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (source->kind == SQLPARSER_SHOW_TABLES) {
    source->values[SHOW_FIELD] = turbodb_text_v(table.name);
    if (source->full) source->values[SHOW_TYPE] = turbodb_text("BASE TABLE");
  } else if (source->kind == SQLPARSER_SHOW_CREATE_TABLE) {
    if (source->position) { source->done = true; *out = NULL; return TURBODB_STATUS_OK; }
    source->values[0] = turbodb_text_v(table.name); source->values[1] = turbodb_text_v(tstr_to_v(source->create_sql));
    ++source->position;
  } else if (source->kind == SQLPARSER_SHOW_INDEX) {
    if (source->position > vec_size(&source->indexes.records)) { source->done = true; *out = NULL; return TURBODB_STATUS_OK; }
    show_index_read(source, &table);
  } else {
    if (source->position == table.count) { source->done = true; *out = NULL; return TURBODB_STATUS_OK; }
    const orm_sql_schema_column *column = &table.columns[source->position];
    const orm_sql_column_default *column_default =
        &table.defaults[source->position];
    const char *type = show_column_type(column->type.kind);
    if (!type) return show_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL SHOW invalid persisted column type");
    source->values[SHOW_FIELD] = turbodb_text_v(column->name);
    source->values[SHOW_TYPE] = turbodb_text(type);
    source->values[SHOW_NULL] = turbodb_text(column->type.nullable ? "YES" : "NO");
    source->values[SHOW_KEY] = turbodb_text(show_keys[*(const uint8_t *)vec_at_const(&source->keys, source->position)]);
    const vstr value = show_default_text(column_default,source->default_text);
    source->values[SHOW_DEFAULT] = value.data ? turbodb_text_v(value) : turbodb_null();
    source->values[SHOW_EXTRA] = turbodb_text("");
    ++source->position;
  }
  *out = source->values; return TURBODB_STATUS_OK;
}
static turbodb_status_t show_next(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_show_source *source = context;
  memset(source->values, 0, sizeof(source->values));
  if (source->failure.status != TURBODB_STATUS_OK) return show_error(error, source->failure.status, source->failure.message);
  if (source->done) { *out = NULL; return TURBODB_STATUS_OK; }
  turbodb_error_t cause; tdsql_error_init(&cause);
  turbodb_status_t status=TURBODB_STATUS_OK;
  const turbodb_value_t *row=NULL;
  do {
    status=show_read(source,&row,&cause);
    if(status!=TURBODB_STATUS_OK || !row || !source->pattern.budget) break;
    turbodb_value_t matched=turbodb_null();
    status=orm_tidesdb_sql_like_eval(&source->like,&row[SHOW_FIELD],
        &source->pattern_value,source->source.budget,&matched,&cause);
    if(status!=TURBODB_STATUS_OK || matched.data.boolean_value) break;
  } while(true);
  if (status != TURBODB_STATUS_OK) {
    source->failure = cause;
    if (status == TURBODB_STATUS_DATASTORE_ERROR) source->owner->failed = true;
    tdsql_error_set(error, status, cause.message);
  }
  else *out=row;
  return status;
}
static turbodb_status_t show_open_context(const sqlparser_document *document,
    orm_sql_catalog_store *store, vstr database_name, const orm_sql_diagnostics *diagnostics,
    orm_sql_session_snapshot session,
    orm_sql_show_source *out, turbodb_error_t *error) {
  if (!document || !out || out->source.budget) return show_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "empty SQL SHOW source required");
  turbodb_status_t status = orm_sql_store_ready(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL || sqlparser_statements(document).count != 1)
    return show_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL SHOW requires one MySQL statement");
  const sqlparser_node *statement = sqlparser_get_node(document, sqlparser_statements(document).first);
  if (!statement || statement->kind != SQLPARSER_SHOW ||
      (statement->as.show.kind != SQLPARSER_SHOW_TABLES && statement->as.show.kind != SQLPARSER_SHOW_COLUMNS &&
       statement->as.show.kind != SQLPARSER_SHOW_INDEX &&
       statement->as.show.kind != SQLPARSER_SHOW_CREATE_TABLE &&
       statement->as.show.kind != SQLPARSER_SHOW_VARIABLES &&
       statement->as.show.kind != SQLPARSER_SHOW_WARNINGS))
    return show_error(error, TURBODB_STATUS_UNSUPPORTED,
        "SQL SHOW requires TABLES, COLUMNS, INDEX, CREATE TABLE, VARIABLES or WARNINGS");
  const bool tables = statement->as.show.kind == SQLPARSER_SHOW_TABLES;
  const bool warnings=statement->as.show.kind==SQLPARSER_SHOW_WARNINGS;
  const bool variables=statement->as.show.kind==SQLPARSER_SHOW_VARIABLES;
  const bool columns=statement->as.show.kind==SQLPARSER_SHOW_COLUMNS;
  const bool filterable=tables||columns||variables||statement->as.show.kind==SQLPARSER_SHOW_INDEX;
  if (variables && !session.valid)
    return show_error(error,TURBODB_STATUS_UNSUPPORTED,"SQL SHOW VARIABLES requires connection context");
  if (statement->as.show.database || (!(variables||tables||columns) && statement->as.show.pattern) ||
      (!filterable && statement->as.show.where) ||
      statement->as.show.extended || (!tables && statement->as.show.full) ||
      (statement->as.show.scope != SQLPARSER_SCOPE_DEFAULT &&
          (!variables || (statement->as.show.scope != SQLPARSER_SCOPE_SESSION && statement->as.show.scope != SQLPARSER_SCOPE_LOCAL))) ||
      (!warnings && (statement->as.show.limit || statement->as.show.count)))
    return show_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL SHOW modifiers or filters are outside the private profile");
  const char *reason = NULL;
  status = orm_sql_name_validate(database_name, &reason);
  if (status != TURBODB_STATUS_OK) return show_error(error, status, reason);
  if (store->active_sources >= SIZE_MAX - 1) return show_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL SHOW source count overflow");
  orm_sql_show_source source = {.source = {.budget = store->budget, .next = show_next},
      .diagnostics=diagnostics,.session=session,.kind = statement->as.show.kind,
      .limit=UINT64_MAX,.full = statement->as.show.full,
      .count=statement->as.show.count};
  if (warnings) status=show_limit_open(document,statement->as.show.limit,
      &source,error);
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_AST_NODES] = sqlparser_node_count(document);
  amount.value[ORM_SQL_BUDGET_PLAN_NODES] = 1; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = sqlparser_node_count(document);
  if (status==TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve_capacity(store->budget, 1, sizeof(source), 0, &source.metadata_bytes, error);
  if (status == TURBODB_STATUS_OK && tables) status = orm_tidesdb_sql_catalog_cursor_open(store, &source.cursor, error);
  if (status==TURBODB_STATUS_OK) status=show_pattern_open(document,statement->as.show.pattern,&source,error);
  if (status == TURBODB_STATUS_OK && !tables && !warnings && !variables) status = show_definition(document, statement->as.show.table, store, &source, error);
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t released = orm_tidesdb_sql_show_close(&source, NULL);
    if (released != TURBODB_STATUS_OK) store->failed = true;
    return released == TURBODB_STATUS_OK ? status : released;
  }
  memcpy(source.label, show_prefix, sizeof(show_prefix) - 1);
  memcpy(source.label + sizeof(show_prefix) - 1, database_name.data, database_name.len);
  source.label[sizeof(show_prefix) - 1 + database_name.len] = '\0';
  source.owner = store; source.source.columns = variables ? SHOW_VARIABLE_COLUMNS : warnings ? (source.count ? 1 : 3) :
      tables ? (source.full ? 2 : 1) :
      (source.kind == SQLPARSER_SHOW_CREATE_TABLE ? 2 :
       source.kind == SQLPARSER_SHOW_INDEX ? ORM_SQL_SHOW_INDEX_COLUMNS : ORM_SQL_SHOW_COLUMNS);
  *out = source; out->source.context = out; out->source.types = out->types;
  static const char *const names[ORM_SQL_SHOW_COLUMNS] = {"Field", "Type", "Null", "Key", "Default", "Extra"};
  static const char *const create_names[] = {"Table", "Create Table"};
  static const char *const warning_names[]={"Level","Code","Message"};
  static const char *const variable_names[]={"Variable_name","Value"};
  static const char *const warning_count_name="@@session.warning_count";
  static const char *const index_names[ORM_SQL_SHOW_INDEX_COLUMNS] = {"Table", "Non_unique", "Key_name", "Seq_in_index",
      "Column_name", "Collation", "Cardinality", "Sub_part", "Packed", "Null", "Index_type", "Comment",
      "Index_comment", "Visible", "Expression"};
  for (size_t i = 0; i < out->source.columns; ++i) {
    if (variables) {
      out->types[i]=(orm_sql_type){TURBODB_VALUE_TEXT,false};
      out->columns[i]=(orm_sql_schema_column){vstr_from_cstr(variable_names[i]),out->types[i]};
      continue;
    }
    if (warnings) {
      const bool number=source.count || i==1;
      out->types[i]=(orm_sql_type){source.count?TURBODB_VALUE_UINT64:
          number?TURBODB_VALUE_INT64:TURBODB_VALUE_TEXT,false};
      out->columns[i]=(orm_sql_schema_column){vstr_from_cstr(source.count?
          warning_count_name:warning_names[i]),out->types[i]};
      continue;
    }
    const bool index = source.kind == SQLPARSER_SHOW_INDEX;
    const bool number = index && (i == SHOW_INDEX_NON_UNIQUE || i == SHOW_INDEX_SEQUENCE ||
        i == SHOW_INDEX_CARDINALITY || i == SHOW_INDEX_SUB_PART);
    const bool nullable = index ? (i == SHOW_INDEX_CARDINALITY || i == SHOW_INDEX_SUB_PART ||
        i == SHOW_INDEX_PACKED || i == SHOW_INDEX_EXPRESSION) : (!tables && i == SHOW_DEFAULT);
    out->types[i] = (orm_sql_type){number ? TURBODB_VALUE_INT64 : TURBODB_VALUE_TEXT, nullable};
    out->columns[i] = (orm_sql_schema_column){vstr_from_cstr(tables ? (i ? "Table_type" : out->label) :
        (source.kind == SQLPARSER_SHOW_CREATE_TABLE ? create_names[i] : index ? index_names[i] : names[i])), out->types[i]};
  }
  out->schema = (orm_sql_table_schema){.columns = out->columns, .count = out->source.columns};
  ++store->active_sources; return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_show_open_evaluation(const sqlparser_document *document,
    orm_sql_catalog_store *store, vstr database_name, orm_sql_evaluation evaluation,
    orm_sql_show_source *out, turbodb_error_t *error) {
  if (evaluation.mode<ORM_SQL_EVALUATION_QUERY || evaluation.mode>ORM_SQL_EVALUATION_IGNORE_WRITE ||
      (evaluation.diagnostics && !evaluation.diagnostics->max_records))
    return show_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL SHOW evaluation context");
  return show_open_context(document,store,database_name,evaluation.diagnostics,evaluation.session,out,error);
}
turbodb_status_t orm_tidesdb_sql_show_open_diagnostics(const sqlparser_document *document,
    orm_sql_catalog_store *store, vstr database_name, const orm_sql_diagnostics *diagnostics,
    orm_sql_show_source *out, turbodb_error_t *error) {
  return show_open_context(document,store,database_name,diagnostics,(orm_sql_session_snapshot){0},out,error);
}
turbodb_status_t orm_tidesdb_sql_show_open(const sqlparser_document *document,
    orm_sql_catalog_store *store, vstr database_name, orm_sql_show_source *out,
    turbodb_error_t *error) {
  return orm_tidesdb_sql_show_open_diagnostics(document,store,database_name,NULL,
      out,error);
}
turbodb_status_t orm_tidesdb_sql_show_close(orm_sql_show_source *source, turbodb_error_t *error) {
  if (!source || !source->source.budget) return TURBODB_STATUS_OK;
  if (source->source.active) return show_error(error, TURBODB_STATUS_BUSY, "SQL SHOW source has an active scan");
  turbodb_status_t status = orm_tidesdb_sql_catalog_cursor_close(&source->cursor, error);
  const turbodb_status_t pattern=orm_tidesdb_sql_expr_destroy(&source->pattern,status==TURBODB_STATUS_OK ? error : NULL);
  if (status==TURBODB_STATUS_OK) status=pattern;
  const turbodb_status_t indexes = orm_sql_index_set_close(&source->indexes, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = indexes;
  const turbodb_status_t keys = orm_sql_work_release(&source->keys, source->key_bytes, source->source.budget,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = keys;
  tstr_freep(&source->create_sql);
  if (source->create_bytes) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(source->source.budget, ORM_SQL_BUDGET_WORK_BYTES,
        source->create_bytes, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  const turbodb_status_t destroyed = orm_tidesdb_sql_catalog_destroy(&source->definition, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = destroyed;
  if (source->metadata_bytes) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(source->source.budget, ORM_SQL_BUDGET_WORK_BYTES,
        source->metadata_bytes, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (source->owner) { --source->owner->active_sources; if (status != TURBODB_STATUS_OK) source->owner->failed = true; }
  *source = (orm_sql_show_source){0}; return status;
}
