#include "connection.h"
#include "schema.h"
#include "scan.h"
#include "runtime.h"
#include "name.h"
#include "work.h"
#include "prepared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <cmeta/cmeta.h>

uint32_t TDSQL_CALL tdsql_abi_version(void) { return TDSQL_ABI_VERSION; }

enum {
  REL_DEFAULT_ROWS = 100000, REL_DEFAULT_NODES = 65536,
  REL_DEFAULT_PAIRS = 1000000, REL_DEFAULT_STEPS = 10000000,
  REL_DEFAULT_WORK = 64 * 1024 * 1024, REL_DEFAULT_TX_BYTES = 256 * 1024 * 1024,
  REL_DEFAULT_TX_ROWS = 1000000, REL_DEFAULT_DEPTH = 64,
  REL_DEFAULT_STACK = 4096, REL_MAX_STACK = 1048576, REL_NUMBER_BYTES = 8,
  REL_DEFAULT_SAVEPOINTS = 64, REL_DEFAULT_WARNINGS = 64,
  REL_MAX_WARNINGS = 65535, REL_CONTROL_NATIVE_PHASES = 2,
  REL_SERIALIZABLE_ORDINAL = 3, REL_DEFAULT_CONNECTIONS = 128,
  REL_DEFAULT_PREPARED_STATEMENTS = 64, REL_DEFAULT_PREPARED_BYTES = 16 * 1024 * 1024
};

typedef struct conn_configuration {
  tstr path, family_name;
  orm_sql_budget_limits budget_limits;
  size_t max_depth, max_stack, max_record, max_savepoints, max_warnings;
  size_t max_connections, max_prepared_statements, max_prepared_bytes;
  uint64_t max_recursive_iterations;
  bool initialize, client_found_rows;
} conn_configuration;

struct tdsql_database {
  orm_tidesdb_database_t *native;
  orm_tidesdb_column_family_t *family;
  conn_configuration config;
  size_t connections;
};
struct tdsql_connection {
  /* Native handles and configuration strings borrow the owning shared database. */
  orm_tidesdb_database_t *database;
  orm_tidesdb_column_family_t *family;
  tdsql_database *shared;
  conn_configuration config;
  orm_sql_diagnostics diagnostics;
  bool private_database, active, autocommit;
  size_t dependents, prepared_count, prepared_bytes;
  turbodb_status_t failure;
  tdsql_transaction *sql_transaction;
  tdsql_transaction *external_transaction;
  sqlparser_transaction_access session_access, next_access;
};
typedef enum conn_transaction_state { REL_ACTIVE, REL_ROLLBACK_REQUIRED, REL_FINISHED } conn_transaction_state;
struct tdsql_transaction {
  tdsql_connection *backend;
  orm_tidesdb_sql_budget budget;
  orm_sql_catalog_store owner;
  size_t references, reserved;
  conn_transaction_state state;
  bool explicit_transaction, read_only;
};
struct tdsql_result {
  tdsql_transaction *transaction;
  tdsql_statement *statement;
  orm_sql_query query;
  orm_sql_scan_row row;
  tdsql_limits limits;
  size_t reserved;
  uint64_t rows, bytes;
  bool terminal, unique_column_names;
  turbodb_error_t error;
  _Alignas(cmeta_capture_storage) unsigned char context_alignment;
};
struct tdsql_statement {
  tdsql_connection *backend;
  orm_sql_prepared metadata;
  tdsql_limits limits;
  size_t charged;
  tdsql_result *result;
};
typedef struct conn_input {
  sqlparser_document *document;
  vec_t parameters;
  size_t reserved;
  orm_sql_prepared *prepared;
} conn_input;

static turbodb_status_t conn_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error, status, message); return status;
}
static orm_sql_evaluation conn_evaluation(tdsql_connection *backend) {
  return (orm_sql_evaluation){.diagnostics=&backend->diagnostics,
      .session={.valid=true,.autocommit=backend->autocommit,
          .read_only=backend->session_access==SQLPARSER_READ_ONLY}};
}
static turbodb_status_t conn_native(turbodb_error_t *error, int code, const char *operation) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "TidesDB relational %s: native error %d", operation, code);
  return conn_error(error, code == ORM_TDB_ERR_MEMORY ? TURBODB_STATUS_OUT_OF_MEMORY :
      code == ORM_TDB_ERR_EXISTS ? TURBODB_STATUS_INVALID_STATE : TURBODB_STATUS_CONNECTION_ERROR, message);
}
static bool conn_equal(vstr text, const char *literal) {
  const size_t size = strlen(literal);
  return text.len == size && (!size || (text.data && !memcmp(text.data, literal, size)));
}
static bool conn_positive(vstr text, uint64_t *out) {
  uint64_t result = 0;
  if (!text.data || !text.len) return false;
  for (size_t i = 0; i < text.len; ++i) {
    const unsigned char c = (unsigned char)text.data[i];
    if (c < '0' || c > '9' || result > (UINT64_MAX - (c - '0')) / 10) return false;
    result = result * 10 + (c - '0');
  }
  if (!result) return false;
  *out = result; return true;
}
static turbodb_status_t conn_settings(conn_configuration *b, const tdsql_config *config,
    const tdsql_limits *limits, turbodb_error_t *error) {
  static const char *const resource_options[ORM_SQL_BUDGET_RESOURCE_COUNT] = {
    "sql_max_work_bytes", "sql_max_materialized_rows", "sql_max_groups", "sql_max_ast_nodes",
    "sql_max_plan_nodes", "sql_max_join_pairs", "sql_max_execution_steps", "sql_max_write_rows",
    "sql_max_write_bytes", "max_scan_rows", "max_scan_bytes"
  };
  static const uint64_t defaults[ORM_SQL_BUDGET_RESOURCE_COUNT] = {
    REL_DEFAULT_WORK, REL_DEFAULT_ROWS, REL_DEFAULT_ROWS, REL_DEFAULT_NODES,
    REL_DEFAULT_NODES, REL_DEFAULT_PAIRS, REL_DEFAULT_STEPS, REL_DEFAULT_ROWS,
    REL_DEFAULT_WORK, REL_DEFAULT_ROWS, REL_DEFAULT_WORK
  };
  memcpy(b->budget_limits.statement.value, defaults, sizeof(defaults));
  b->budget_limits.transaction = (orm_sql_transaction_budget_amount){REL_DEFAULT_TX_ROWS, REL_DEFAULT_TX_BYTES, REL_DEFAULT_TX_BYTES};
  b->max_depth = REL_DEFAULT_DEPTH; b->max_stack = REL_DEFAULT_STACK;
  b->max_savepoints = REL_DEFAULT_SAVEPOINTS;
  b->max_warnings = REL_DEFAULT_WARNINGS;
  b->max_record = limits->max_parameter_bytes;
  b->max_connections = REL_DEFAULT_CONNECTIONS;
  b->max_prepared_statements = REL_DEFAULT_PREPARED_STATEMENTS;
  b->max_prepared_bytes = REL_DEFAULT_PREPARED_BYTES;
  for (uint32_t i = 0; i < config->option_count; ++i) {
    const turbodb_option_t *o = &config->options[i];
    if (!o->keyword.data || !o->value.data)
      return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational connection option");
    for (uint32_t j = 0; j < i; ++j)
      if (o->keyword.len == config->options[j].keyword.len &&
          !memcmp(o->keyword.data, config->options[j].keyword.data, o->keyword.len))
        return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "duplicate relational connection option");
    if (conn_equal(o->keyword, "sql_profile")) {
      if (!conn_equal(o->value, "relational")) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL profile");
    } else if (conn_equal(o->keyword, "path") || conn_equal(o->keyword, "column_family")) {
      const bool family = conn_equal(o->keyword, "column_family");
      const char *reason = NULL;
      if (!o->value.len || o->value.len > limits->max_query_bytes || memchr(o->value.data, 0, o->value.len) ||
          (family && orm_sql_name_validate(o->value, &reason) != TURBODB_STATUS_OK))
        return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational path or column_family");
      tstr *destination = family ? &b->family_name : &b->path;
      *destination = tstr_from_v(o->value);
      if (!*destination) return conn_error(error, TURBODB_STATUS_OUT_OF_MEMORY, "copy relational connection option");
    } else if (conn_equal(o->keyword, "sql_initialize")) {
      if (conn_equal(o->value, "true") || conn_equal(o->value, "1")) b->initialize = true;
      else if (conn_equal(o->value, "false") || conn_equal(o->value, "0")) b->initialize = false;
      else return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "sql_initialize must be true, false, 1 or 0");
    } else if (conn_equal(o->keyword, "sql_client_found_rows")) {
      if (conn_equal(o->value, "true") || conn_equal(o->value, "1")) b->client_found_rows = true;
      else if (conn_equal(o->value, "false") || conn_equal(o->value, "0")) b->client_found_rows = false;
      else return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
          "sql_client_found_rows must be true, false, 1 or 0");
    } else {
      uint64_t value = 0; bool known = false;
      if (!conn_positive(o->value, &value))
        return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "relational limits must be positive decimal integers");
      for (size_t r = 0; r < ORM_SQL_BUDGET_RESOURCE_COUNT; ++r)
        if (conn_equal(o->keyword, resource_options[r])) { b->budget_limits.statement.value[r] = value; known = true; break; }
      if (known) continue;
      if (conn_equal(o->keyword, "sql_max_connections")) {
        if (value > SIZE_MAX) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
            "relational connection capacity exceeds size bound");
        b->max_connections = (size_t)value;
      }
      else if (conn_equal(o->keyword, "sql_max_prepared_statements") || conn_equal(o->keyword, "sql_max_prepared_bytes")) {
        if (value > SIZE_MAX) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "prepared capacity exceeds size bound");
        if (conn_equal(o->keyword, "sql_max_prepared_statements")) b->max_prepared_statements = (size_t)value;
        else b->max_prepared_bytes = (size_t)value;
      }
      else if (conn_equal(o->keyword, "sql_max_transaction_read_rows")) b->budget_limits.transaction.read_rows = value;
      else if (conn_equal(o->keyword, "sql_max_transaction_read_bytes")) b->budget_limits.transaction.read_bytes = value;
      else if (conn_equal(o->keyword, "sql_max_transaction_write_bytes")) b->budget_limits.transaction.write_bytes = value;
      else if (conn_equal(o->keyword, "sql_max_recursive_iterations")) b->max_recursive_iterations = value;
      else if (conn_equal(o->keyword, "sql_max_warnings")) {
        if (value>REL_MAX_WARNINGS) return conn_error(error,
            TURBODB_STATUS_INVALID_ARGUMENT,
            "sql_max_warnings exceeds the connection bound");
        b->max_warnings=(size_t)value;
      }
      else if (conn_equal(o->keyword, "sql_max_savepoints")) {
        if (value > INT_MAX / 2 - 1) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "relational savepoint capacity exceeds native bound");
        b->max_savepoints = (size_t)value;
      } else if (conn_equal(o->keyword, "sql_max_depth") || conn_equal(o->keyword, "sql_max_stack_entries") || conn_equal(o->keyword, "sql_max_record_bytes")) {
        if (value > SIZE_MAX) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "relational size limit overflow");
        if (conn_equal(o->keyword, "sql_max_depth")) b->max_depth = (size_t)value;
        else if (conn_equal(o->keyword, "sql_max_stack_entries")) b->max_stack = (size_t)value;
        else b->max_record = (size_t)value;
      } else return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "unknown relational connection option");
    }
  }
  if (!b->path || !b->family_name)
    return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "relational requires explicit path and column_family");
  if (b->max_stack < 2 || b->max_stack > REL_MAX_STACK ||
      b->budget_limits.statement.value[ORM_SQL_BUDGET_AST_NODES] > UINT32_MAX ||
      b->budget_limits.statement.value[ORM_SQL_BUDGET_AST_NODES] > SIZE_MAX / sizeof(sqlparser_node))
    return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational parser limits");
  orm_tidesdb_sql_budget check;
  return orm_tidesdb_sql_budget_init(&check, &b->budget_limits, error);
}

static turbodb_status_t conn_available(tdsql_connection *b, turbodb_error_t *error) {
  if (b->failure != TURBODB_STATUS_OK) return conn_error(error, b->failure, "relational connection requires close after cleanup failure");
  return b->active ? conn_error(error, TURBODB_STATUS_BUSY, "relational connection already owns a transaction or query") : TURBODB_STATUS_OK;
}
turbodb_status_t TDSQL_CALL tdsql_transaction_prepare(tdsql_transaction *t, bool command, turbodb_error_t *error) {
  if (!t) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational transaction");
  if (t->state != REL_ACTIVE || t->owner.failed)
    return conn_error(error, TURBODB_STATUS_INVALID_STATE, "relational transaction requires rollback");
  if (t->budget.statement_active)
    return conn_error(error, TURBODB_STATUS_BUSY, "relational transaction already has an active statement");
  return command ? orm_sql_diagnostics_reset(&t->backend->diagnostics, error) : TURBODB_STATUS_OK;
}
turbodb_status_t TDSQL_CALL tdsql_connection_prepare(tdsql_connection *b, bool command, turbodb_error_t *error) {
  if (!b) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational connection");
  if (b->failure != TURBODB_STATUS_OK)
    return conn_error(error, b->failure, "relational connection requires close after terminal failure");
  if ((b->active && !b->sql_transaction) ||
      (b->sql_transaction && b->sql_transaction->budget.statement_active))
    return conn_error(error, TURBODB_STATUS_BUSY, "close relational query or ORM transaction before executing a connection command");
  if (!command && b->sql_transaction)
    return tdsql_transaction_prepare(b->sql_transaction, false, error);
  return command ? orm_sql_diagnostics_reset(&b->diagnostics, error) : TURBODB_STATUS_OK;
}
static turbodb_status_t conn_statement_begin(tdsql_transaction *t, turbodb_error_t *error) {
  if (t->state != REL_ACTIVE || t->owner.failed)
    return conn_error(error, TURBODB_STATUS_INVALID_STATE, "relational transaction requires rollback");
  return orm_tidesdb_sql_budget_begin(&t->budget, error);
}
static turbodb_status_t conn_statement_end(tdsql_transaction *t, turbodb_error_t *error) {
  const turbodb_status_t status = orm_tidesdb_sql_budget_end(&t->budget, error);
  if (status != TURBODB_STATUS_OK) t->owner.failed = true;
  return status;
}
static bool conn_show_warnings(const sqlparser_document *document) {
  if (!document || sqlparser_statements(document).count!=1) return false;
  const sqlparser_node *node=sqlparser_get_node(document,
      sqlparser_statements(document).first);
  return node && node->kind==SQLPARSER_SHOW &&
      node->as.show.kind==SQLPARSER_SHOW_WARNINGS;
}
turbodb_status_t TDSQL_CALL tdsql_transaction_finish(tdsql_transaction *t, bool commit, turbodb_error_t *error) {
  if (!t) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational transaction");
  if (t->budget.statement_active)
    return conn_error(error, TURBODB_STATUS_BUSY, "close relational query before finishing its transaction");
  if (t->state == REL_FINISHED || (commit && t->state == REL_ROLLBACK_REQUIRED))
    return conn_error(error, TURBODB_STATUS_INVALID_STATE, "relational transaction is terminal or requires rollback");
  const turbodb_status_t status = orm_tidesdb_sql_catalog_finish(&t->owner, commit, error);
  if (status == TURBODB_STATUS_OK || status == TURBODB_STATUS_COMMIT_UNKNOWN) {
    t->state = REL_FINISHED; t->backend->active = false;
    if (t->backend->external_transaction == t) t->backend->external_transaction = NULL;
    if (status == TURBODB_STATUS_COMMIT_UNKNOWN) t->backend->failure = status;
  } else if (!t->owner.transaction) t->state = REL_ROLLBACK_REQUIRED;
  return status;
}
turbodb_status_t TDSQL_CALL tdsql_transaction_release_checked(tdsql_transaction *t, turbodb_error_t *error) {
  if (!t || --t->references) return TURBODB_STATUS_OK;
  turbodb_status_t status = orm_tidesdb_sql_catalog_finish(&t->owner, false, error);
  if (status != TURBODB_STATUS_OK) t->backend->failure = status;
  const turbodb_status_t released = orm_tidesdb_sql_budget_release_retained(&t->budget, t->reserved,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (released != TURBODB_STATUS_OK) {
    t->backend->failure = TURBODB_STATUS_INTERNAL_ERROR;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  /* A finished handle can coexist with the connection's next transaction. */
  if (t->state != REL_FINISHED) t->backend->active = false;
  if (t->backend->external_transaction == t) t->backend->external_transaction = NULL;
  --t->backend->dependents;
  free(t);
  return status;
}
void TDSQL_CALL tdsql_transaction_release(tdsql_transaction *t) { (void)tdsql_transaction_release_checked(t, NULL); }

static turbodb_status_t conn_request_check(const tdsql_request *request, turbodb_error_t *error) {
  if (!request) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "missing TidesSQL request");
  if (request->struct_size < sizeof(*request) || request->abi_version != TDSQL_ABI_VERSION)
    return conn_error(error, TURBODB_STATUS_ABI_MISMATCH, "incompatible TidesSQL request ABI");
  return TURBODB_STATUS_OK;
}
static turbodb_status_t conn_transaction_new(tdsql_connection *b, tdsql_transaction **out, turbodb_error_t *error) {
  turbodb_status_t status = conn_available(b, error);
  orm_tidesdb_sql_budget budget; size_t reserved = 0;
  if (status != TURBODB_STATUS_OK) return status;
  status = orm_tidesdb_sql_budget_init(&budget, &b->config.budget_limits, error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_begin(&budget, error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, 1, sizeof(tdsql_transaction), 0, &reserved, error);
  if (status != TURBODB_STATUS_OK) return status;
  tdsql_transaction *t = calloc(1, sizeof(*t));
  if (!t) return conn_error(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate relational transaction");
  ++b->dependents;
  t->backend = b; t->budget = budget; t->reserved = reserved; t->references = 1;
  status = orm_tidesdb_sql_catalog_begin(b->database, b->family, b->config.max_record, &t->budget, &t->owner, error);
  if (status == TURBODB_STATUS_OK) status = conn_statement_end(t, error);
  if (status != TURBODB_STATUS_OK) { tdsql_transaction_release(t); return status; }
  b->active = true; *out = t; return TURBODB_STATUS_OK;
}
static turbodb_status_t conn_transaction_start(tdsql_connection *b, sqlparser_transaction_access access,
    tdsql_transaction **out, turbodb_error_t *error) {
  tdsql_transaction *t = NULL;
  const turbodb_status_t status = conn_transaction_new(b, &t, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (access == SQLPARSER_ACCESS_DEFAULT)
    access = b->next_access != SQLPARSER_ACCESS_DEFAULT ? b->next_access : b->session_access;
  t->read_only = access == SQLPARSER_READ_ONLY;
  b->next_access = SQLPARSER_ACCESS_DEFAULT;
  *out = t; return TURBODB_STATUS_OK;
}

static turbodb_status_t conn_input_parse(const tdsql_connection *b, const tdsql_request *request,
    const tdsql_limits *limits, size_t max_nodes, conn_input *input, turbodb_error_t *error) {
  if (!request || !request->sql.data || !request->sql.len ||
      (request->parameter_count && !request->read_parameter))
    return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational SQL request");
  if (request->sql.len > limits->max_query_bytes || request->parameter_count > limits->max_parameters)
    return conn_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "relational query or parameter limit exceeded");
  const sqlparser_options options = {SQLPARSER_MYSQL, true};
  const sqlparser_limits bounds = {limits->max_query_bytes, max_nodes, 1, b->config.max_stack};
  sqlparser_error diagnostic;
  const sqlparser_status parsed = sqlparser_parse_with_options(request->sql.data,
      request->sql.len, &options, &bounds, &input->document, &diagnostic);
  if (parsed != SQLPARSER_OK) {
    char message[TURBODB_ERROR_MESSAGE_CAPACITY];
    (void)snprintf(message, sizeof(message), "TidesDB relational SQL at byte %zu: %s", diagnostic.offset, diagnostic.message);
    return conn_error(error, parsed == SQLPARSER_OUT_OF_MEMORY ? TURBODB_STATUS_OUT_OF_MEMORY :
        parsed == SQLPARSER_LIMIT_EXCEEDED ? TURBODB_STATUS_LIMIT_EXCEEDED : TURBODB_STATUS_SQL_ERROR, message);
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t conn_input_parameters(tdsql_transaction *t, const tdsql_request *request,
    conn_input *input, turbodb_error_t *error) {
  const size_t count = request->parameter_count;
  turbodb_status_t status = orm_sql_work_zero(&input->parameters, count,
      sizeof(turbodb_value_t), _Alignof(turbodb_value_t), &t->budget, &input->reserved, error);
  if (status == TURBODB_STATUS_OK)
    for (size_t i = 0; i < count; ++i)
      *(turbodb_value_t *)vec_at(&input->parameters, i) = request->read_parameter(request->parameter_context, i);
  return status;
}
static void conn_input_discard(conn_input *input) {
  sqlparser_document_destroy(input->document);
  input->document = NULL;
}
static turbodb_status_t conn_input_close(tdsql_transaction *t, conn_input *input, turbodb_error_t *error) {
  conn_input_discard(input);
  return orm_sql_work_release(&input->parameters, input->reserved, &t->budget, error);
}

typedef struct conn_sql_characteristics {
  sqlparser_transaction_access access;
  sqlparser_scope scope;
  size_t nodes;
} conn_sql_characteristics;
static const char conn_next_characteristics_error[]=
    "cannot change next transaction characteristics while a transaction is in progress";
static turbodb_status_t conn_characteristics_bind(const sqlparser_document *document,
    const sqlparser_node *node, size_t parameters, conn_sql_characteristics *out, turbodb_error_t *error) {
  if (parameters)
    return conn_error(error, TURBODB_STATUS_SQL_ERROR, "SET TRANSACTION accepts no parameters");
  if (node->as.transaction.scope == SQLPARSER_SCOPE_GLOBAL ||
      (node->as.transaction.isolation != SQLPARSER_ISOLATION_DEFAULT &&
       node->as.transaction.isolation != SQLPARSER_SERIALIZABLE))
    return conn_error(error, TURBODB_STATUS_UNSUPPORTED, "SET TRANSACTION supports session/next SERIALIZABLE characteristics only");
  if (node->as.transaction.mode != SQLPARSER_TRANSACTION_DEFAULT || node->as.transaction.name ||
      node->as.transaction.consistent_snapshot || node->as.transaction.chain != SQLPARSER_CHOICE_UNSPECIFIED ||
      node->as.transaction.release != SQLPARSER_CHOICE_UNSPECIFIED)
    return conn_error(error, TURBODB_STATUS_UNSUPPORTED, "unsupported SET TRANSACTION characteristic");
  *out = (conn_sql_characteristics){node->as.transaction.access, node->as.transaction.scope,
      sqlparser_node_count(document)};
  return TURBODB_STATUS_OK;
}
static turbodb_status_t conn_characteristics_apply(tdsql_connection *b, tdsql_transaction *t,
    const conn_sql_characteristics *characteristics, turbodb_error_t *error) {
  if (characteristics->scope == SQLPARSER_SCOPE_DEFAULT && t)
    return conn_error(error, TURBODB_STATUS_SQL_ERROR, conn_next_characteristics_error);
  if (characteristics->access != SQLPARSER_ACCESS_DEFAULT) {
    if (characteristics->scope == SQLPARSER_SCOPE_DEFAULT) b->next_access = characteristics->access;
    else {
      b->session_access = characteristics->access;
      if (!t) b->next_access = SQLPARSER_ACCESS_DEFAULT;
    }
  }
  return TURBODB_STATUS_OK;
}
static orm_sql_budget_amount conn_control_amount(size_t nodes) {
  orm_sql_budget_amount amount = {0};
  amount.value[ORM_SQL_BUDGET_AST_NODES] = nodes;
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = nodes + REL_CONTROL_NATIVE_PHASES;
  return amount;
}
static turbodb_status_t conn_sql_set_transaction(tdsql_transaction *t, const conn_input *input,
    const sqlparser_node *statement, turbodb_error_t *error) {
  conn_sql_characteristics characteristics;
  turbodb_status_t status = conn_characteristics_bind(input->document, statement,
      vec_size(&input->parameters), &characteristics, error);
  if (status == TURBODB_STATUS_OK) {
    const orm_sql_budget_amount amount = conn_control_amount(characteristics.nodes);
    status = orm_tidesdb_sql_budget_reserve(&t->budget, &amount, error);
  }
  return status == TURBODB_STATUS_OK ? conn_characteristics_apply(t->backend, t, &characteristics, error) : status;
}
typedef enum conn_savepoint_operation { REL_SAVEPOINT_CREATE, REL_SAVEPOINT_ROLLBACK, REL_SAVEPOINT_RELEASE } conn_savepoint_operation;
static turbodb_status_t conn_savepoint_apply(tdsql_transaction *t, vstr name,
    conn_savepoint_operation operation, turbodb_error_t *error) {
  switch (operation) {
    case REL_SAVEPOINT_CREATE:
      return orm_tidesdb_sql_catalog_savepoint(&t->owner, name, t->backend->config.max_savepoints, error);
    case REL_SAVEPOINT_ROLLBACK:
      return orm_tidesdb_sql_catalog_rollback_to(&t->owner, name, error);
    case REL_SAVEPOINT_RELEASE:
      return orm_tidesdb_sql_catalog_release_savepoint(&t->owner, name, error);
    default: return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational savepoint operation");
  }
}
static turbodb_status_t conn_sql_savepoint(tdsql_transaction *t, const conn_input *input,
    const sqlparser_node *statement, turbodb_error_t *error) {
  conn_savepoint_operation operation;
  switch (statement->as.transaction.kind) {
    case SQLPARSER_SAVEPOINT: operation = REL_SAVEPOINT_CREATE; break;
    case SQLPARSER_ROLLBACK_TO: operation = REL_SAVEPOINT_ROLLBACK; break;
    case SQLPARSER_RELEASE: operation = REL_SAVEPOINT_RELEASE; break;
    default: return conn_error(error, TURBODB_STATUS_UNSUPPORTED,
        "SQL transaction lifecycle requires the ORM transaction API");
  }
  if (vec_size(&input->parameters))
    return conn_error(error, TURBODB_STATUS_SQL_ERROR, "SQL savepoint commands accept no parameters");
  vstr name = {0}; const char *reason = NULL;
  turbodb_status_t status = orm_sql_name_node(input->document, statement->as.transaction.name, &name, &reason);
  if (status != TURBODB_STATUS_OK) return conn_error(error, status, reason);
  orm_sql_budget_amount amount = {0};
  amount.value[ORM_SQL_BUDGET_AST_NODES] = sqlparser_node_count(input->document);
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = name.len + 1;
  status = orm_tidesdb_sql_budget_reserve(&t->budget, &amount, error);
  if (status != TURBODB_STATUS_OK) return status;
  /* MySQL SAVEPOINT outside a multi-statement transaction has no effect. */
  if (operation == REL_SAVEPOINT_CREATE && !t->explicit_transaction) return TURBODB_STATUS_OK;
  return conn_savepoint_apply(t, name, operation, error);
}
static bool conn_statement_writes(const sqlparser_document *document, const sqlparser_node *statement) {
  const sqlparser_node *body = statement && statement->kind == SQLPARSER_WITH ?
      sqlparser_get_node(document, statement->as.with.body) : statement;
  if (!body) return false;
  switch (body->kind) {
    case SQLPARSER_INSERT: case SQLPARSER_UPDATE: case SQLPARSER_DELETE:
    case SQLPARSER_CREATE_TABLE: case SQLPARSER_DROP_TABLE: case SQLPARSER_ALTER_TABLE:
    case SQLPARSER_TRUNCATE_TABLE: case SQLPARSER_CREATE_INDEX: case SQLPARSER_DROP_INDEX:
      return true;
    default: return false;
  }
}
static turbodb_status_t conn_run_command(tdsql_transaction *t, const conn_input *input,
    size_t *result, turbodb_error_t *error) {
  const sqlparser_node *statement = sqlparser_get_node(input->document,
      sqlparser_statements(input->document).first);
  if (t->read_only && conn_statement_writes(input->document, statement))
    return conn_error(error, TURBODB_STATUS_SQL_ERROR, "cannot execute a write in a READ ONLY transaction");
  if (statement && statement->kind == SQLPARSER_TRANSACTION)
    return statement->as.transaction.kind == SQLPARSER_SET_TRANSACTION ?
        conn_sql_set_transaction(t, input, statement, error) : conn_sql_savepoint(t, input, statement, error);
  return orm_sql_runtime_execute_evaluation(input->document, &t->owner,
      vec_data_const(&input->parameters), vec_size(&input->parameters), t->backend->config.max_depth,
      t->backend->config.max_recursive_iterations, t->backend->config.client_found_rows,
      conn_evaluation(t->backend), result, error);
}
static turbodb_status_t conn_set_characteristics_prepare(tdsql_transaction *t, const conn_input *input,
    const sqlparser_node *statement, conn_sql_characteristics *out, turbodb_error_t *error);
static turbodb_status_t conn_execute_prepared(tdsql_transaction *t, const tdsql_request *request,
    conn_input *input, uint64_t *affected, turbodb_error_t *error) {
  size_t result = 0;
  conn_sql_characteristics characteristics={0};
  const sqlparser_node *statement=sqlparser_get_node(input->document,sqlparser_statements(input->document).first);
  const bool set_characteristics=statement && statement->kind==SQLPARSER_SET;
  turbodb_status_t status = conn_statement_begin(t, error);
  if (status != TURBODB_STATUS_OK) { conn_input_discard(input); return status; }
  if (input->prepared) status = orm_sql_prepared_check(input->prepared, &t->owner, error);
  if (status == TURBODB_STATUS_OK) status = conn_input_parameters(t, request, input, error);
  if (status == TURBODB_STATUS_OK) status = set_characteristics ?
      conn_set_characteristics_prepare(t,input,statement,&characteristics,error) :
      conn_run_command(t, input, &result, error);
  turbodb_status_t cleanup = conn_input_close(t, input, status == TURBODB_STATUS_OK ? error : NULL);
  if (cleanup == TURBODB_STATUS_OK) cleanup = conn_statement_end(t, status == TURBODB_STATUS_OK ? error : NULL);
  if (cleanup != TURBODB_STATUS_OK) { t->owner.failed = true; return conn_error(error, cleanup, "relational command cleanup failed; rollback required"); }
  if (status==TURBODB_STATUS_OK && set_characteristics)
    status=conn_characteristics_apply(t->backend,t,&characteristics,error);
  if (status == TURBODB_STATUS_OK) *affected = result;
  return status;
}
turbodb_status_t TDSQL_CALL tdsql_transaction_execute(tdsql_transaction *t, const tdsql_request *request,
    uint64_t *affected, turbodb_error_t *error) {
  if (!t || !request || !affected) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational request");
  const turbodb_status_t admission = conn_request_check(request, error);
  if (admission != TURBODB_STATUS_OK) return admission;
  const tdsql_limits *limits = &request->limits;
  conn_input input = {0};
  if (t->state != REL_ACTIVE || t->owner.failed)
    return conn_error(error, TURBODB_STATUS_INVALID_STATE, "relational transaction requires rollback");
  if (t->budget.statement_active)
    return conn_error(error, TURBODB_STATUS_BUSY, "relational transaction already has an active statement");
  turbodb_status_t status = orm_sql_diagnostics_reset(&t->backend->diagnostics, error);
  if (status == TURBODB_STATUS_OK) status = conn_input_parse(t->backend, request, limits,
      (size_t)t->budget.limits.statement.value[ORM_SQL_BUDGET_AST_NODES], &input, error);
  if (status == TURBODB_STATUS_OK) status = conn_execute_prepared(t, request, &input, affected, error);
  conn_input_discard(&input);
  return status;
}


static turbodb_status_t conn_result_bytes(tdsql_result *c, uint64_t *out) {
  uint64_t bytes = 0;
  for (size_t i = 0; i < c->row.count; ++i) {
    orm_sql_schema_column column;
    turbodb_status_t status = orm_tidesdb_sql_runtime_column(&c->query, i, &column, &c->error);
    if (status != TURBODB_STATUS_OK) return status;
    const turbodb_value_t *v = &c->row.values[i];
    const uint64_t size = v->kind == TURBODB_VALUE_NULL ? 0 : v->kind == TURBODB_VALUE_BOOLEAN ? 1 :
      v->kind == TURBODB_VALUE_TEXT ? v->data.text_value.len : v->kind == TURBODB_VALUE_BLOB ? v->data.blob_value.size : REL_NUMBER_BYTES;
    if (column.name.len > UINT64_MAX - bytes || size > UINT64_MAX - bytes - column.name.len)
      return conn_error(&c->error, TURBODB_STATUS_LIMIT_EXCEEDED, "relational result byte overflow");
    bytes += column.name.len + size;
  }
  if (bytes > c->limits.max_parameter_bytes || bytes > c->limits.max_result_bytes - c->bytes)
    return conn_error(&c->error, TURBODB_STATUS_LIMIT_EXCEEDED, "relational result byte limit exceeded");
  *out = bytes; return TURBODB_STATUS_OK;
}
static tdsql_row conn_row(orm_sql_scan_row row) {
  return (tdsql_row){row.state == ORM_SQL_SCAN_ROW ? TDSQL_ROW :
      row.state == ORM_SQL_SCAN_CANCELLED ? TDSQL_CANCELLED : TDSQL_DONE, row.values, row.count};
}
turbodb_status_t TDSQL_CALL tdsql_result_next(tdsql_result *c, tdsql_row *out, turbodb_error_t *error) {
  if (!c || !out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational result request");
  if (c->terminal) { *out = (tdsql_row){.state=TDSQL_DONE}; return TURBODB_STATUS_OK; }
  c->row = (orm_sql_scan_row){0};
  turbodb_status_t status = orm_tidesdb_sql_runtime_next(&c->query, &c->row, &c->error);
  if (status == TURBODB_STATUS_OK && c->row.state != ORM_SQL_SCAN_ROW) {
    c->terminal = true; *out = conn_row(c->row); return status;
  }
  uint64_t bytes = 0;
  if (status == TURBODB_STATUS_OK && c->rows == c->limits.max_result_rows)
    status = conn_error(&c->error, TURBODB_STATUS_LIMIT_EXCEEDED, "relational result row limit exceeded");
  if (status == TURBODB_STATUS_OK) status = conn_result_bytes(c, &bytes);
  if (status != TURBODB_STATUS_OK) {
    c->terminal = true; return conn_error(error, status, c->error.message);
  }
  ++c->rows; c->bytes += bytes; *out = conn_row(c->row);
  return TURBODB_STATUS_OK;
}

turbodb_status_t TDSQL_CALL tdsql_result_cancel(tdsql_result *c, turbodb_error_t *error) {
  if (!c) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational result");
  c->terminal = true; c->row = (orm_sql_scan_row){0};
  return orm_tidesdb_sql_runtime_cancel(&c->query, error);
}
turbodb_status_t TDSQL_CALL tdsql_result_destroy_checked(tdsql_result *c, turbodb_error_t *error) {
  if (!c) return TURBODB_STATUS_OK;
  tdsql_transaction *t = c->transaction;
  turbodb_status_t status = orm_tidesdb_sql_runtime_close(&c->query, error);
  const turbodb_status_t released = orm_tidesdb_sql_budget_release(&t->budget, ORM_SQL_BUDGET_WORK_BYTES, c->reserved,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (c->statement) c->statement->result = NULL;
  free(c);
  const turbodb_status_t ended = conn_statement_end(t, status == TURBODB_STATUS_OK ? error : NULL);
  if (status != TURBODB_STATUS_OK || ended != TURBODB_STATUS_OK) {
    t->owner.failed = true; t->backend->failure = status != TURBODB_STATUS_OK ? status : ended;
  }
  if (status == TURBODB_STATUS_OK) status = ended;
  const turbodb_status_t finished = tdsql_transaction_release_checked(t, status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? finished : status;
}
void TDSQL_CALL tdsql_result_destroy(tdsql_result *c) { (void)tdsql_result_destroy_checked(c, NULL); }
turbodb_status_t TDSQL_CALL tdsql_result_column(tdsql_result *result, size_t index,
    tdsql_column *out, turbodb_error_t *error) {
  if (!result || !out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational column request");
  orm_sql_schema_column column;
  const turbodb_status_t status = orm_tidesdb_sql_runtime_column(&result->query, index, &column, error);
  if (status == TURBODB_STATUS_OK) *out = (tdsql_column){column.name, column.type.kind, column.type.nullable};
  return status;
}
size_t TDSQL_CALL tdsql_result_columns(const tdsql_result *result) { return result ? result->query.columns : 0; }
void *TDSQL_CALL tdsql_result_context(tdsql_result *result) { return result ? result + 1 : NULL; }

static turbodb_status_t conn_check_columns(tdsql_result *c, turbodb_error_t *error) {
  if (c->query.columns > c->limits.max_columns)
    return conn_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "relational result column limit exceeded");
  if (!c->unique_column_names) return TURBODB_STATUS_OK;
  /* Map readers require unique names. O(C^2), bounded by max_columns; charge
   * each comparison so unusually wide results cannot bypass execution limits. */
  for (size_t i = 0; i < c->query.columns; ++i) {
    orm_sql_schema_column a;
    turbodb_status_t status = orm_tidesdb_sql_runtime_column(&c->query, i, &a, error);
    if (status != TURBODB_STATUS_OK) return status;
    for (size_t j = 0; j < i; ++j) {
      orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
      status = orm_tidesdb_sql_budget_reserve(&c->transaction->budget, &amount, error);
      if (status != TURBODB_STATUS_OK) return status;
      orm_sql_schema_column b;
      status = orm_tidesdb_sql_runtime_column(&c->query, j, &b, error);
      if (status != TURBODB_STATUS_OK) return status;
      if (a.name.len == b.name.len && !memcmp(a.name.data, b.name.data, a.name.len))
        return conn_error(error, TURBODB_STATUS_UNSUPPORTED, "relational row maps require unique column names; use aliases");
    }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t conn_open_prepared(tdsql_transaction *t, const tdsql_request *request,
    const tdsql_limits *limits, conn_input *input, tdsql_result **out, turbodb_error_t *error) {
  size_t reserved = 0;
  turbodb_status_t status = conn_statement_begin(t, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (input->prepared) status = orm_sql_prepared_check(input->prepared, &t->owner, error);
  size_t bytes = 0;
  if (status == TURBODB_STATUS_OK && request->result_context_bytes > SIZE_MAX - sizeof(tdsql_result))
    status = conn_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "relational result context size overflow");
  if (status == TURBODB_STATUS_OK) {
    bytes = sizeof(tdsql_result) + request->result_context_bytes;
    status = orm_tidesdb_sql_budget_reserve_capacity(&t->budget, 1, bytes, 0, &reserved, error);
  }
  tdsql_result *c = status == TURBODB_STATUS_OK ? calloc(1, bytes) : NULL;
  if (status == TURBODB_STATUS_OK && !c) status = conn_error(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate relational cursor");
  if (c) {
    c->transaction = t; c->reserved = reserved; c->limits = *limits; c->unique_column_names = request->unique_column_names; tdsql_error_init(&c->error);
    ++t->references;
    status = conn_input_parameters(t, request, input, error);
    if (status == TURBODB_STATUS_OK) {
      const tdsql_connection *b=t->backend;
      const bool warnings=conn_show_warnings(input->document);
      if (!warnings)
        status=orm_sql_diagnostics_reset(&t->backend->diagnostics,error);
      if (status==TURBODB_STATUS_OK)
        status=orm_sql_runtime_open_evaluation(input->document,&t->owner,
            vstr_from_cstr(b->config.family_name),vec_data(&input->parameters),
            vec_size(&input->parameters),b->config.max_depth,b->config.max_recursive_iterations,
            conn_evaluation(t->backend),&c->query,error);
    }
    if (status == TURBODB_STATUS_OK) status = conn_check_columns(c, error);
  }
  const turbodb_status_t cleanup = conn_input_close(t, input, status == TURBODB_STATUS_OK ? error : NULL);
  if (cleanup != TURBODB_STATUS_OK) { t->owner.failed = true; status = conn_error(error, cleanup, "relational input cleanup failed"); }
  if (status != TURBODB_STATUS_OK) {
    if (c) tdsql_result_destroy(c);
    else {
      if (reserved && orm_tidesdb_sql_budget_release(&t->budget, ORM_SQL_BUDGET_WORK_BYTES, reserved, NULL) != TURBODB_STATUS_OK) t->owner.failed = true;
      if (conn_statement_end(t, NULL) != TURBODB_STATUS_OK) t->owner.failed = true;
    }
    return status;
  }
  *out = c;
  return TURBODB_STATUS_OK;
}
turbodb_status_t TDSQL_CALL tdsql_transaction_query(tdsql_transaction *t, const tdsql_request *request,
    tdsql_result **out, turbodb_error_t *error) {
  if (out) *out = NULL;
  if (!t || !request || !out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational request");
  const turbodb_status_t admission = conn_request_check(request, error);
  if (admission != TURBODB_STATUS_OK) return admission;
  const tdsql_limits *limits = &request->limits;
  conn_input input = {0};
  if (t->state != REL_ACTIVE || t->owner.failed)
    return conn_error(error, TURBODB_STATUS_INVALID_STATE, "relational transaction requires rollback");
  if (t->budget.statement_active)
    return conn_error(error, TURBODB_STATUS_BUSY, "relational transaction already has an active statement");
  turbodb_status_t status = conn_input_parse(t->backend, request, limits,
      (size_t)t->budget.limits.statement.value[ORM_SQL_BUDGET_AST_NODES], &input, error);
  if (status == TURBODB_STATUS_OK) status = conn_open_prepared(t, request, limits, &input, out, error);
  conn_input_discard(&input);
  return status;
}
turbodb_status_t TDSQL_CALL tdsql_transaction_savepoint(tdsql_transaction *t, vstr name,
    tdsql_savepoint_operation operation, turbodb_error_t *error) {
  if (!t) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational transaction");
  if (operation < TDSQL_SAVEPOINT_CREATE || operation > TDSQL_SAVEPOINT_RELEASE)
    return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational savepoint operation");
  turbodb_status_t status = conn_statement_begin(t, error);
  if (status != TURBODB_STATUS_OK) return status;
  status = orm_sql_diagnostics_reset(&t->backend->diagnostics, error);
  if (status == TURBODB_STATUS_OK) status = conn_savepoint_apply(t, name, (conn_savepoint_operation)operation, error);
  const turbodb_status_t ended = conn_statement_end(t, status == TURBODB_STATUS_OK ? error : NULL);
  return ended == TURBODB_STATUS_OK ? status : conn_error(error, ended, "relational savepoint cleanup failed; full rollback required");
}
turbodb_status_t TDSQL_CALL tdsql_connection_begin(tdsql_connection *b, tdsql_transaction **out, turbodb_error_t *error) {
  if (out) *out = NULL;
  if (!b || !out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational transaction request");
  tdsql_transaction *t = NULL;
  const turbodb_status_t status = conn_transaction_start(b, SQLPARSER_ACCESS_DEFAULT, &t, error);
  if (status == TURBODB_STATUS_OK) {
    t->explicit_transaction = true; b->external_transaction = t; *out = t;
  }
  return status;
}

typedef enum conn_control_kind { REL_CONTROL_NONE, REL_CONTROL_BEGIN, REL_CONTROL_COMMIT, REL_CONTROL_ROLLBACK,
  REL_CONTROL_CHARACTERISTICS, REL_CONTROL_VARIABLE } conn_control_kind;
typedef struct conn_sql_control {
  conn_control_kind kind;
  size_t nodes;
  bool chain;
  sqlparser_transaction_access access;
  conn_sql_characteristics characteristics;
  sqlparser_id value;
  orm_sql_session_variable variable;
  bool boolean_value;
} conn_sql_control;
static bool conn_sql_equal(vstr text, const char *lowercase) {
  const size_t length = strlen(lowercase);
  if (!text.data || text.len != length) return false;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)text.data[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c != (unsigned char)lowercase[i]) return false;
  }
  return true;
}
static turbodb_status_t conn_set_bind(const conn_input *input, const sqlparser_node *node,
    conn_sql_control *out, turbodb_error_t *error) {
  if (node->as.set.kind != SQLPARSER_SET_ASSIGNMENTS || node->as.set.assignments.count != 1)
    return conn_error(error, TURBODB_STATUS_UNSUPPORTED, "SET supports one implemented system variable assignment only");
  const sqlparser_node *assignment = sqlparser_get_node(input->document, node->as.set.assignments.first);
  const sqlparser_node *name = assignment ? sqlparser_get_node(input->document, assignment->as.assignment.name) : NULL;
  if (!assignment || assignment->kind != SQLPARSER_ASSIGNMENT || !name)
    return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SET assignment AST");
  if (assignment->as.assignment.scope == SQLPARSER_SCOPE_GLOBAL)
    return conn_error(error, TURBODB_STATUS_UNSUPPORTED, "SET GLOBAL system variables are not supported");
  vstr text = {sqlparser_text(input->document, name->span), name->span.length}, identifier = {0};
  if (name->kind == SQLPARSER_VARIABLE) {
    if (text.len < 2 || text.data[0] != '@' || text.data[1] != '@')
      return conn_error(error, TURBODB_STATUS_UNSUPPORTED, "SET user variables are not supported");
    text.data += 2; text.len -= 2;
  } else if (name->kind != SQLPARSER_NAME || name->as.name.parts != 1)
    return conn_error(error, TURBODB_STATUS_UNSUPPORTED, "SET requires an unqualified system variable");
  const char *reason = NULL;
  turbodb_status_t status = orm_sql_name_part(&text, &identifier, &reason);
  if (status != TURBODB_STATUS_OK) return conn_error(error, status, reason);
  orm_sql_session_variable variable;
  if (text.len || !orm_sql_session_find(identifier,&variable))
    return conn_error(error, TURBODB_STATUS_UNSUPPORTED, "unsupported SET system variable");
  /* Only bare @@transaction_* denotes the next transaction; bare names and
   * explicit SESSION/LOCAL assignments set the session default. */
  const sqlparser_scope scope=variable!=ORM_SQL_SESSION_AUTOCOMMIT && name->kind==SQLPARSER_VARIABLE ?
      SQLPARSER_SCOPE_DEFAULT : SQLPARSER_SCOPE_SESSION;
  *out = (conn_sql_control){.kind=REL_CONTROL_VARIABLE,.variable=variable,
      .characteristics={.scope=scope},.nodes=sqlparser_node_count(input->document),
      .value=assignment->as.assignment.value};
  return TURBODB_STATUS_OK;
}
static turbodb_status_t conn_control_bind(const conn_input *input, const tdsql_request *request,
    conn_sql_control *out, turbodb_error_t *error) {
  const sqlparser_node *node = sqlparser_get_node(input->document,
      sqlparser_statements(input->document).first);
  if (node && node->kind == SQLPARSER_SET) return conn_set_bind(input, node, out, error);
  if (!node || node->kind != SQLPARSER_TRANSACTION) return TURBODB_STATUS_OK;
  conn_sql_control control = {.nodes = sqlparser_node_count(input->document)};
  const size_t count = request->parameter_count;
  switch (node->as.transaction.kind) {
    case SQLPARSER_BEGIN: case SQLPARSER_START_TRANSACTION: control.kind = REL_CONTROL_BEGIN; break;
    case SQLPARSER_COMMIT: control.kind = REL_CONTROL_COMMIT; break;
    case SQLPARSER_ROLLBACK: control.kind = REL_CONTROL_ROLLBACK; break;
    case SQLPARSER_SET_TRANSACTION: {
      const turbodb_status_t status = conn_characteristics_bind(input->document, node, count, &control.characteristics, error);
      if (status != TURBODB_STATUS_OK) return status;
      control.kind = REL_CONTROL_CHARACTERISTICS; *out = control; return TURBODB_STATUS_OK;
    }
    default: return TURBODB_STATUS_OK;
  }
  if (count) return conn_error(error, TURBODB_STATUS_SQL_ERROR, "SQL transaction controls accept no parameters");
  if (node->as.transaction.mode != SQLPARSER_TRANSACTION_DEFAULT ||
      node->as.transaction.name || node->as.transaction.isolation != SQLPARSER_ISOLATION_DEFAULT ||
      node->as.transaction.scope != SQLPARSER_SCOPE_DEFAULT || node->as.transaction.consistent_snapshot ||
      node->as.transaction.release == SQLPARSER_CHOICE_YES)
    return conn_error(error, TURBODB_STATUS_UNSUPPORTED, "unsupported SQL transaction characteristic or session release");
  if (control.kind == REL_CONTROL_BEGIN) {
    if (node->as.transaction.chain != SQLPARSER_CHOICE_UNSPECIFIED ||
        node->as.transaction.release != SQLPARSER_CHOICE_UNSPECIFIED)
      return conn_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL BEGIN accepts no completion clauses");
    control.access = node->as.transaction.access;
  } else {
    if (node->as.transaction.access != SQLPARSER_ACCESS_DEFAULT)
      return conn_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL transaction completion accepts no access mode");
    control.chain = node->as.transaction.chain == SQLPARSER_CHOICE_YES;
  }
  *out = control; return TURBODB_STATUS_OK;
}
static bool conn_boolean_word(vstr text, bool *out) {
  if (conn_sql_equal(text, "on")) { *out = true; return true; }
  if (conn_sql_equal(text, "off")) { *out = false; return true; }
  return false;
}
static turbodb_status_t conn_boolean_convert(orm_sql_session_variable variable,
    const turbodb_value_t *value, bool *out, turbodb_error_t *error) {
  if (value->kind == TURBODB_VALUE_BOOLEAN) { *out = value->data.boolean_value != 0; return TURBODB_STATUS_OK; }
  if (value->kind == TURBODB_VALUE_INT64 && value->data.int64_value >= 0 && value->data.int64_value <= 1) {
    *out = value->data.int64_value != 0; return TURBODB_STATUS_OK;
  }
  if (value->kind == TURBODB_VALUE_UINT64 && value->data.uint64_value <= 1) {
    *out = value->data.uint64_value != 0; return TURBODB_STATUS_OK;
  }
  if (value->kind == TURBODB_VALUE_TEXT && conn_boolean_word(value->data.text_value, out)) return TURBODB_STATUS_OK;
  return conn_error(error, TURBODB_STATUS_SQL_ERROR, variable==ORM_SQL_SESSION_AUTOCOMMIT ?
      "autocommit requires integer 0/1 or ON/OFF" : "transaction_read_only requires integer 0/1 or ON/OFF");
}
static turbodb_status_t conn_isolation_convert(const turbodb_value_t *value, turbodb_error_t *error) {
  if (value->kind==TURBODB_VALUE_TEXT) {
    if (conn_sql_equal(value->data.text_value,"serializable")) return TURBODB_STATUS_OK;
    if (conn_sql_equal(value->data.text_value,"read-uncommitted") ||
        conn_sql_equal(value->data.text_value,"read-committed") ||
        conn_sql_equal(value->data.text_value,"repeatable-read"))
      return conn_error(error,TURBODB_STATUS_UNSUPPORTED,"transaction_isolation supports SERIALIZABLE only");
  }
  if (value->kind==TURBODB_VALUE_INT64 || value->kind==TURBODB_VALUE_UINT64) {
    if ((value->kind==TURBODB_VALUE_INT64 && value->data.int64_value==REL_SERIALIZABLE_ORDINAL) ||
        (value->kind==TURBODB_VALUE_UINT64 && value->data.uint64_value==REL_SERIALIZABLE_ORDINAL)) return TURBODB_STATUS_OK;
    if ((value->kind==TURBODB_VALUE_INT64 && value->data.int64_value>=0 && value->data.int64_value<REL_SERIALIZABLE_ORDINAL) ||
        (value->kind==TURBODB_VALUE_UINT64 && value->data.uint64_value<REL_SERIALIZABLE_ORDINAL))
      return conn_error(error,TURBODB_STATUS_UNSUPPORTED,"transaction_isolation supports SERIALIZABLE only");
  }
  return conn_error(error,TURBODB_STATUS_SQL_ERROR,"transaction_isolation requires SERIALIZABLE or its integer ordinal 3");
}
static turbodb_status_t conn_set_evaluate(tdsql_transaction *t, const conn_input *input,
    const conn_sql_control *control, bool *out, turbodb_error_t *error) {
  const sqlparser_id root=control->value;
  orm_tidesdb_sql_budget *budget = &t->budget;
  vec_t offsets={0}, types={0}, slots={0}, values={0};
  size_t offset_bytes=0, type_bytes=0, slot_bytes=0, value_bytes=0;
  orm_sql_expr program={0}; orm_sql_expr_run run={0}; bool result=false;
  const size_t count = vec_size(&input->parameters);
  turbodb_status_t status = orm_sql_bind_parameter_offsets(input->document, count, budget, &offsets, &offset_bytes, error);
  const sqlparser_node *node = sqlparser_get_node(input->document, root);
  if (status == TURBODB_STATUS_OK && !node) status = conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "missing SET expression");
  bool special = false;
  if (status == TURBODB_STATUS_OK && node->kind == SQLPARSER_DEFAULT_VALUE) {
    special = true; result = control->variable==ORM_SQL_SESSION_AUTOCOMMIT;
  }
  if (status == TURBODB_STATUS_OK && node->kind == SQLPARSER_NAME) {
    vstr name={0}; const char *reason=NULL;
    status = orm_sql_name_node(input->document, root, &name, &reason);
    if (status == TURBODB_STATUS_OK) special = control->variable==ORM_SQL_SESSION_TRANSACTION_ISOLATION ?
        conn_sql_equal(name,"serializable") : conn_boolean_word(name, &result);
    else conn_error(error, status, reason);
  }
  if (status == TURBODB_STATUS_OK && !special) {
    status = orm_sql_work_zero(&types, count, sizeof(orm_sql_type), _Alignof(orm_sql_type), budget, &type_bytes, error);
    if (status == TURBODB_STATUS_OK) for (size_t i=0; i<count; ++i) {
      const turbodb_value_t *value=vec_at_const(&input->parameters,i);
      *(orm_sql_type *)vec_at(&types,i)=(orm_sql_type){value->kind,value->kind==TURBODB_VALUE_NULL};
    }
    const orm_sql_table_schema empty={0};
    const orm_sql_binding_scope scope={.document=input->document,.schema=&empty,.hidden_input=true,
      .parameter_types=vec_data_const(&types),.parameter_offsets=vec_data_const(&offsets),
      .parameter_count=count,.budget=budget};
    if (status == TURBODB_STATUS_OK) status = orm_sql_bind_expression(&scope, root, t->backend->config.max_depth,
        (orm_sql_expression_target){.program=&program,.slots=&slots,.slot_bytes=&slot_bytes}, false, error);
    const size_t inputs=vec_size(&slots);
    if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&values, inputs, sizeof(turbodb_value_t), _Alignof(turbodb_value_t), budget, &value_bytes, error);
    if (status == TURBODB_STATUS_OK) for (size_t i=0; i<inputs; ++i)
      *(turbodb_value_t *)vec_at(&values,i)=*(const turbodb_value_t *)vec_at_const(&input->parameters,*(const size_t *)vec_at_const(&slots,i));
    orm_sql_evaluation evaluation=conn_evaluation(t->backend);
    evaluation.mode=ORM_SQL_EVALUATION_WRITE;
    evaluation.diagnostics=NULL;
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_expr_run_open_evaluation(&program,
        evaluation, &run, error);
    turbodb_value_t value=turbodb_null();
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_expr_run_eval(&run, vec_data_const(&values), inputs, &value, error);
    if (status == TURBODB_STATUS_OK) status = control->variable==ORM_SQL_SESSION_TRANSACTION_ISOLATION ?
        conn_isolation_convert(&value,error) : conn_boolean_convert(control->variable,&value, &result, error);
  }
  turbodb_status_t cleanup=orm_tidesdb_sql_expr_run_close(&run,status==TURBODB_STATUS_OK ? error : NULL);
  const turbodb_status_t destroyed=orm_tidesdb_sql_expr_destroy(&program,cleanup==TURBODB_STATUS_OK && status==TURBODB_STATUS_OK ? error : NULL);
  if (destroyed!=TURBODB_STATUS_OK) cleanup=destroyed;
  vec_t *vectors[]={&offsets,&types,&slots,&values};
  const size_t bytes[]={offset_bytes,type_bytes,slot_bytes,value_bytes};
  for (size_t i=0; i<sizeof(vectors)/sizeof(vectors[0]); ++i) {
    const turbodb_status_t released=orm_sql_work_release(vectors[i],bytes[i],budget,cleanup==TURBODB_STATUS_OK && status==TURBODB_STATUS_OK ? error : NULL);
    if (released!=TURBODB_STATUS_OK) cleanup=released;
  }
  if (cleanup!=TURBODB_STATUS_OK) { t->owner.failed=true; return conn_error(error,cleanup,"SET expression cleanup failed"); }
  if (status==TURBODB_STATUS_OK) *out=result;
  return status;
}
static void conn_set_result(conn_sql_control *control,bool result) {
  control->boolean_value=result;
  if (control->variable==ORM_SQL_SESSION_TRANSACTION_READ_ONLY)
    control->characteristics.access=result?SQLPARSER_READ_ONLY:SQLPARSER_READ_WRITE;
}
static turbodb_status_t conn_set_characteristics_prepare(tdsql_transaction *t, const conn_input *input,
    const sqlparser_node *statement, conn_sql_characteristics *out, turbodb_error_t *error) {
  conn_sql_control control={0};
  turbodb_status_t status=conn_set_bind(input,statement,&control,error);
  if (status==TURBODB_STATUS_OK && control.variable==ORM_SQL_SESSION_AUTOCOMMIT)
    return conn_error(error,TURBODB_STATUS_UNSUPPORTED,"SET autocommit requires the connection command API");
  if (status==TURBODB_STATUS_OK && control.characteristics.scope==SQLPARSER_SCOPE_DEFAULT)
    return conn_error(error,TURBODB_STATUS_SQL_ERROR,conn_next_characteristics_error);
  if (status==TURBODB_STATUS_OK) {
    const orm_sql_budget_amount amount=conn_control_amount(control.nodes);
    status=orm_tidesdb_sql_budget_reserve(&t->budget,&amount,error);
  }
  bool result=false;
  if (status==TURBODB_STATUS_OK) status=conn_set_evaluate(t,input,&control,&result,error);
  if (status==TURBODB_STATUS_OK) {
    conn_set_result(&control,result); *out=control.characteristics;
  }
  return status;
}
static turbodb_status_t conn_set_prepare(tdsql_connection *b, const tdsql_request *request,
    conn_input *input, conn_sql_control *control, turbodb_error_t *error) {
  tdsql_transaction local={.backend=b}, *t=b->sql_transaction ? b->sql_transaction : &local;
  if (t!=&local && (t->state!=REL_ACTIVE || t->owner.failed))
    return conn_error(error, TURBODB_STATUS_INVALID_STATE, "SQL transaction requires full ROLLBACK");
  if (t!=&local && control->variable!=ORM_SQL_SESSION_AUTOCOMMIT &&
      control->characteristics.scope==SQLPARSER_SCOPE_DEFAULT)
    return conn_error(error,TURBODB_STATUS_SQL_ERROR,conn_next_characteristics_error);
  turbodb_status_t status=t==&local ? orm_tidesdb_sql_budget_init(&t->budget,&b->config.budget_limits,error) : TURBODB_STATUS_OK;
  if (status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_begin(&t->budget,error);
  if (status!=TURBODB_STATUS_OK) return status;
  const orm_sql_budget_amount amount=conn_control_amount(control->nodes);
  status=orm_tidesdb_sql_budget_reserve(&t->budget,&amount,error);
  if (status==TURBODB_STATUS_OK) status=conn_input_parameters(t,request,input,error);
  bool result=false;
  if (status==TURBODB_STATUS_OK) status=conn_set_evaluate(t,input,control,&result,error);
  turbodb_status_t cleanup=conn_input_close(t,input,status==TURBODB_STATUS_OK ? error : NULL);
  const turbodb_status_t ended=conn_statement_end(t,cleanup==TURBODB_STATUS_OK && status==TURBODB_STATUS_OK ? error : NULL);
  if (ended!=TURBODB_STATUS_OK) cleanup=ended;
  if (cleanup!=TURBODB_STATUS_OK) { t->owner.failed=true; return conn_error(error,cleanup,"SET preparation cleanup failed"); }
  if (status==TURBODB_STATUS_OK) conn_set_result(control,result);
  return status;
}
static turbodb_status_t conn_control_admit(tdsql_connection *b, const conn_sql_control *control,
    turbodb_error_t *error) {
  tdsql_transaction *t = b->sql_transaction;
  if (t && control->kind != REL_CONTROL_ROLLBACK && (t->state != REL_ACTIVE || t->owner.failed))
    return conn_error(error, TURBODB_STATUS_INVALID_STATE, "SQL transaction requires full ROLLBACK");
  orm_tidesdb_sql_budget local;
  turbodb_status_t status = t ? TURBODB_STATUS_OK : orm_tidesdb_sql_budget_init(&local, &b->config.budget_limits, error);
  orm_tidesdb_sql_budget *budget = t ? &t->budget : &local;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_begin(budget, error);
  if (status != TURBODB_STATUS_OK) return status;
  const orm_sql_budget_amount amount = conn_control_amount(control->nodes);
  status = orm_tidesdb_sql_budget_reserve(budget, &amount, error);
  const turbodb_status_t ended = orm_tidesdb_sql_budget_end(budget, status == TURBODB_STATUS_OK ? error : NULL);
  if (ended != TURBODB_STATUS_OK) {
    if (t) t->owner.failed = true;
    return conn_error(error, ended, "SQL transaction control budget cleanup failed");
  }
  return status;
}
static turbodb_status_t conn_sql_end(tdsql_connection *b, bool commit, turbodb_error_t *error) {
  tdsql_transaction *t = b->sql_transaction;
  if (!t) return TURBODB_STATUS_OK;
  const turbodb_status_t status = tdsql_transaction_finish(t, commit, error);
  if (status == TURBODB_STATUS_COMMIT_UNKNOWN) b->failure = status;
  if (t->state == REL_FINISHED) {
    b->sql_transaction = NULL;
    tdsql_transaction_release(t);
    if (status == TURBODB_STATUS_OK && b->failure != TURBODB_STATUS_OK)
      return conn_error(error, b->failure, "SQL transaction final release failed; close connection");
  }
  return status;
}
static turbodb_status_t conn_sql_begin(tdsql_connection *b, sqlparser_transaction_access access, turbodb_error_t *error) {
  tdsql_transaction *t = NULL;
  const turbodb_status_t status = conn_transaction_start(b, access, &t, error);
  if (status == TURBODB_STATUS_OK) {
    t->explicit_transaction = true;
    b->sql_transaction = t;
  }
  return status;
}
static turbodb_status_t conn_sql_lifecycle(tdsql_connection *b, const conn_sql_control *control,
    turbodb_error_t *error) {
  if (control->kind == REL_CONTROL_VARIABLE) {
    if (control->variable!=ORM_SQL_SESSION_AUTOCOMMIT)
      return conn_characteristics_apply(b,b->sql_transaction,&control->characteristics,error);
    if (b->autocommit == control->boolean_value) return TURBODB_STATUS_OK;
    const turbodb_status_t status=control->boolean_value ? conn_sql_end(b,true,error) : TURBODB_STATUS_OK;
    if (status==TURBODB_STATUS_OK) {
      b->autocommit=control->boolean_value;
      if (b->autocommit) b->next_access=SQLPARSER_ACCESS_DEFAULT;
    }
    return status;
  }
  turbodb_status_t status = conn_control_admit(b, control, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (control->kind == REL_CONTROL_CHARACTERISTICS)
    return conn_characteristics_apply(b, b->sql_transaction, &control->characteristics, error);
  sqlparser_transaction_access access = control->kind == REL_CONTROL_BEGIN ? control->access : SQLPARSER_ACCESS_DEFAULT;
  if (access == SQLPARSER_ACCESS_DEFAULT && b->sql_transaction)
    access = b->sql_transaction->read_only ? SQLPARSER_READ_ONLY : SQLPARSER_READ_WRITE;
  const bool completing = b->sql_transaction != NULL || control->kind != REL_CONTROL_BEGIN;
  status = conn_sql_end(b, control->kind != REL_CONTROL_ROLLBACK, error);
  if (status == TURBODB_STATUS_OK && (control->kind == REL_CONTROL_BEGIN || control->chain)) {
    status = conn_sql_begin(b, access, error);
    if (status != TURBODB_STATUS_OK && completing) {
      char message[TURBODB_ERROR_MESSAGE_CAPACITY];
      (void)snprintf(message, sizeof(message), "previous SQL transaction ended; new transaction failed: %s",
          error && error->message[0] ? error->message : "transaction start failed");
      conn_error(error, status, message);
    }
  }
  if (status == TURBODB_STATUS_OK && control->kind != REL_CONTROL_BEGIN && !control->chain)
    b->next_access = SQLPARSER_ACCESS_DEFAULT;
  return status;
}
static turbodb_status_t conn_command_available(tdsql_connection *b, turbodb_error_t *error) {
  if (b->failure != TURBODB_STATUS_OK)
    return conn_error(error, b->failure, "relational connection requires close after terminal failure");
  if ((b->active && !b->sql_transaction) ||
      (b->sql_transaction && b->sql_transaction->budget.statement_active))
    return conn_error(error, TURBODB_STATUS_BUSY, "close relational query or ORM transaction before executing a connection command");
  return TURBODB_STATUS_OK;
}
static turbodb_status_t conn_query_available(tdsql_connection *b, turbodb_error_t *error) {
  if (b->failure != TURBODB_STATUS_OK)
    return conn_error(error, b->failure, "relational connection requires close after terminal failure");
  tdsql_transaction *t = b->sql_transaction;
  if (!t) return conn_available(b, error);
  if (t->state != REL_ACTIVE || t->owner.failed)
    return conn_error(error, TURBODB_STATUS_INVALID_STATE, "relational transaction requires rollback");
  if (t->budget.statement_active)
    return conn_error(error, TURBODB_STATUS_BUSY, "relational transaction already has an active statement");
  return TURBODB_STATUS_OK;
}
static size_t conn_max_nodes(const tdsql_connection *b) {
  return (size_t)(b->sql_transaction ?
      b->sql_transaction->budget.limits.statement.value[ORM_SQL_BUDGET_AST_NODES] :
      b->config.budget_limits.statement.value[ORM_SQL_BUDGET_AST_NODES]);
}
static turbodb_status_t conn_execute_input(tdsql_connection *b, const tdsql_request *request,
    conn_input *input, uint64_t *affected, turbodb_error_t *error) {
  conn_sql_control control = {0};
  turbodb_status_t status = conn_control_bind(input, request, &control, error);
  if (status != TURBODB_STATUS_OK || control.kind != REL_CONTROL_NONE) {
    if (status == TURBODB_STATUS_OK && control.kind == REL_CONTROL_VARIABLE)
      status=conn_set_prepare(b,request,input,&control,error);
    conn_input_discard(input);
    if (status == TURBODB_STATUS_OK) status = conn_sql_lifecycle(b, &control, error);
    if (status == TURBODB_STATUS_OK) *affected = 0;
    return status;
  }
  const bool automatic = b->sql_transaction == NULL && b->autocommit;
  tdsql_transaction *t = b->sql_transaction; uint64_t result = 0;
  if (automatic) status = conn_transaction_start(b, SQLPARSER_ACCESS_DEFAULT, &t, error);
  else if (!t) { status=conn_sql_begin(b,SQLPARSER_ACCESS_DEFAULT,error); t=b->sql_transaction; }
  if (status == TURBODB_STATUS_OK) status = conn_execute_prepared(t, request, input, &result, error);
  conn_input_discard(input);
  if (automatic && status == TURBODB_STATUS_OK) status = tdsql_transaction_finish(t, true, error);
  if (automatic && t && status != TURBODB_STATUS_OK && t->state != REL_FINISHED) {
    turbodb_error_t cleanup; tdsql_error_init(&cleanup);
    const turbodb_status_t rolled = tdsql_transaction_finish(t, false, &cleanup);
    if (rolled != TURBODB_STATUS_OK) status = conn_error(error, rolled, cleanup.message);
  }
  if (automatic) tdsql_transaction_release(t);
  if (status == TURBODB_STATUS_OK) *affected = result;
  return status;
}

static turbodb_status_t conn_query_input(tdsql_connection *b, const tdsql_request *request,
    conn_input *input, tdsql_result **out, turbodb_error_t *error) {
  tdsql_transaction *t = b->sql_transaction;
  const bool automatic = t == NULL && b->autocommit;
  turbodb_status_t status = TURBODB_STATUS_OK;
  if (!t) {
    if (automatic) status = conn_transaction_start(b, SQLPARSER_ACCESS_DEFAULT, &t, error);
    else { status = conn_sql_begin(b, SQLPARSER_ACCESS_DEFAULT, error); t = b->sql_transaction; }
  }
  if (status == TURBODB_STATUS_OK) status = conn_open_prepared(t, request, &request->limits, input, out, error);
  conn_input_discard(input);
  if (automatic) tdsql_transaction_release(t);
  return status;
}

turbodb_status_t TDSQL_CALL tdsql_connection_execute(tdsql_connection *b, const tdsql_request *request,
    uint64_t *affected, turbodb_error_t *error) {
  if (!b || !request || !affected) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational request");
  turbodb_status_t status = conn_request_check(request, error);
  if (status == TURBODB_STATUS_OK) status = conn_command_available(b, error);
  if (status != TURBODB_STATUS_OK) return status;
  conn_input input = {0};
  status = orm_sql_diagnostics_reset(&b->diagnostics, error);
  if (status == TURBODB_STATUS_OK) status = conn_input_parse(b, request, &request->limits, conn_max_nodes(b), &input, error);
  if (status == TURBODB_STATUS_OK) status = conn_execute_input(b, request, &input, affected, error);
  conn_input_discard(&input);
  return status;
}

turbodb_status_t TDSQL_CALL tdsql_connection_query(tdsql_connection *b, const tdsql_request *request,
    tdsql_result **out, turbodb_error_t *error) {
  if (out) *out = NULL;
  if (!b || !request || !out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational request");
  turbodb_status_t status = conn_request_check(request, error);
  if (status == TURBODB_STATUS_OK) status = conn_query_available(b, error);
  if (status != TURBODB_STATUS_OK) return status;
  conn_input input = {0};
  status = conn_input_parse(b, request, &request->limits, conn_max_nodes(b), &input, error);
  if (status == TURBODB_STATUS_OK) status = conn_query_input(b, request, &input, out, error);
  conn_input_discard(&input);
  return status;
}

static turbodb_status_t conn_response_kind(const sqlparser_document *document,
    tdsql_response_kind *out, turbodb_error_t *error) {
  const sqlparser_node *node = sqlparser_get_node(document, sqlparser_statements(document).first);
  /* WITH wrappers are bounded by the document's admitted node count. */
  for (size_t i = 0; node && i < sqlparser_node_count(document); ++i) {
    if (node->kind == SQLPARSER_WITH) { node = sqlparser_get_node(document, node->as.with.body); continue; }
    switch (node->kind) {
      case SQLPARSER_SELECT: case SQLPARSER_SHOW: case SQLPARSER_EXPLAIN:
      case SQLPARSER_UNION: case SQLPARSER_QUERY_GROUP: *out = TDSQL_ROWS; break;
      default: *out = TDSQL_COMMAND; break;
    }
    return TURBODB_STATUS_OK;
  }
  return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational statement body");
}

static turbodb_status_t conn_dispatch_input(tdsql_connection *b, const tdsql_request *request,
    conn_input *input, tdsql_response *out, turbodb_error_t *error) {
  tdsql_response response = tdsql_response_default();
  turbodb_status_t status = conn_response_kind(input->document, &response.kind, error);
  if (status == TURBODB_STATUS_OK && response.kind == TDSQL_ROWS) {
    status = conn_query_available(b, error);
    if (status == TURBODB_STATUS_OK) status = conn_query_input(b, request, input, &response.result, error);
  } else if (status == TURBODB_STATUS_OK) {
    status = orm_sql_diagnostics_reset(&b->diagnostics, error);
    if (status == TURBODB_STATUS_OK) status = conn_execute_input(b, request, input, &response.affected_rows, error);
  }
  conn_input_discard(input);
  if (status == TURBODB_STATUS_OK) *out = response;
  return status;
}

turbodb_status_t TDSQL_CALL tdsql_connection_run(tdsql_connection *b, const tdsql_request *request,
    tdsql_response *out, turbodb_error_t *error) {
  if (!b || !request || !out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid unified SQL request");
  if (out->struct_size < sizeof(*out) || out->abi_version != TDSQL_ABI_VERSION)
    return conn_error(error, TURBODB_STATUS_ABI_MISMATCH, "incompatible TidesSQL response ABI");
  turbodb_status_t status = conn_request_check(request, error);
  if (status == TURBODB_STATUS_OK) status = conn_command_available(b, error);
  conn_input input = {0};
  if (status == TURBODB_STATUS_OK) status = conn_input_parse(b, request, &request->limits, conn_max_nodes(b), &input, error);
  if (status == TURBODB_STATUS_OK) status = conn_dispatch_input(b, request, &input, out, error);
  conn_input_discard(&input);
  return status;
}

/* Metadata snapshots never enter the SQL transaction/access-mode lifecycle. */
turbodb_status_t TDSQL_CALL tdsql_connection_statement_prepare(tdsql_connection *b,
    const tdsql_request *request, tdsql_statement **out, turbodb_error_t *error) {
  if (out) *out = NULL;
  if (!b || !request || !out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid statement prepare request");
  turbodb_status_t status = conn_request_check(request, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (request->parameter_count || request->read_parameter || request->parameter_context)
    return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "prepare accepts SQL and limits without parameter values");
  status = conn_query_available(b, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (b->prepared_count >= b->config.max_prepared_statements || b->dependents == SIZE_MAX ||
      b->prepared_bytes > b->config.max_prepared_bytes ||
      b->config.max_prepared_bytes - b->prepared_bytes < sizeof(tdsql_statement))
    return conn_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "prepared session capacity exceeded");
  conn_input input = {0};
  status = conn_input_parse(b, request, &request->limits, conn_max_nodes(b), &input, error);
  if (status != TURBODB_STATUS_OK) { conn_input_discard(&input); return status; }
  tdsql_statement *statement = calloc(1, sizeof(*statement));
  if (!statement) { conn_input_discard(&input); return conn_error(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate prepared statement"); }
  orm_tidesdb_sql_budget budget = {0}; orm_sql_catalog_store snapshot = {0};
  tdsql_transaction *transaction = b->sql_transaction;
  orm_sql_catalog_store *owner = transaction ? &transaction->owner : &snapshot;
  bool begun = false, begin_cleanup_failed = false;
  if (transaction) status = conn_statement_begin(transaction, error);
  else {
    status = orm_tidesdb_sql_budget_init(&budget, &b->config.budget_limits, error);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_begin(&budget, error);
  }
  begun = status == TURBODB_STATUS_OK;
  if (begun && !transaction) status = orm_sql_catalog_begin_checked(b->database, b->family,
      b->config.max_record, &budget, &snapshot, &begin_cleanup_failed, error);
  if (begin_cleanup_failed) b->failure = status;
  orm_sql_budget_limits metadata_limits = b->config.budget_limits;
  const size_t remaining = b->config.max_prepared_bytes - b->prepared_bytes;
  if (metadata_limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] > remaining)
    metadata_limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = remaining;
  if (status == TURBODB_STATUS_OK) status = orm_sql_prepared_open(input.document, request->sql,
      owner, &metadata_limits, &request->limits, b->config.max_depth,
      sizeof(*statement), &statement->metadata, error);
  conn_input_discard(&input);
  turbodb_status_t cleanup = TURBODB_STATUS_OK;
  if (snapshot.transaction) cleanup = orm_tidesdb_sql_catalog_finish(&snapshot, false,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (begun) {
    const turbodb_status_t ended = transaction ? conn_statement_end(transaction, cleanup == TURBODB_STATUS_OK && status == TURBODB_STATUS_OK ? error : NULL) :
        orm_tidesdb_sql_budget_end(&budget, cleanup == TURBODB_STATUS_OK && status == TURBODB_STATUS_OK ? error : NULL);
    if (cleanup == TURBODB_STATUS_OK) cleanup = ended;
  }
  if (cleanup != TURBODB_STATUS_OK) {
    b->failure = cleanup;
    status = conn_error(error, cleanup, "prepared metadata cleanup failed; close connection");
  }
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t closed = orm_sql_prepared_close(&statement->metadata, NULL);
    free(statement);
    if (closed != TURBODB_STATUS_OK) { b->failure = closed; return conn_error(error, closed, "prepared ownership cleanup failed"); }
    return status;
  }
  statement->backend = b; statement->limits = request->limits;
  statement->charged = (size_t)statement->metadata.budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
  ++b->prepared_count; ++b->dependents; b->prepared_bytes += statement->charged;
  *out = statement; return TURBODB_STATUS_OK;
}

size_t TDSQL_CALL tdsql_statement_parameters(const tdsql_statement *statement) {
  return statement ? vec_size(&statement->metadata.types) : 0;
}
size_t TDSQL_CALL tdsql_statement_columns(const tdsql_statement *statement) {
  return statement ? vec_size(&statement->metadata.columns) : 0;
}
static turbodb_status_t conn_statement_valid(const tdsql_statement *statement, turbodb_error_t *error) {
  if (!statement) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid prepared statement");
  if (statement->metadata.invalidated)
    return conn_error(error, TURBODB_STATUS_INVALID_STATE, "prepared schema invalidated; close and prepare again");
  return TURBODB_STATUS_OK;
}
turbodb_status_t TDSQL_CALL tdsql_statement_parameter(const tdsql_statement *statement,
    size_t index, tdsql_column *out, turbodb_error_t *error) {
  if (!out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid prepared parameter output");
  turbodb_status_t status = conn_statement_valid(statement, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (index >= tdsql_statement_parameters(statement))
    return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "prepared parameter index out of range");
  const orm_sql_type *type = vec_at_const(&statement->metadata.types, index);
  *out = (tdsql_column){.name={"?",1},.kind=type->kind,.nullable=type->nullable};
  return TURBODB_STATUS_OK;
}
turbodb_status_t TDSQL_CALL tdsql_statement_column(const tdsql_statement *statement,
    size_t index, tdsql_column *out, turbodb_error_t *error) {
  if (!out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid prepared column output");
  turbodb_status_t status = conn_statement_valid(statement, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (index >= tdsql_statement_columns(statement))
    return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "prepared column index out of range");
  const orm_sql_schema_column *column = vec_at_const(&statement->metadata.columns, index);
  *out = (tdsql_column){column->name,column->type.kind,column->type.nullable};
  return TURBODB_STATUS_OK;
}
static turbodb_value_t TDSQL_CALL conn_bound_value(const void *context, size_t index) {
  return ((const turbodb_value_t *)context)[index];
}
turbodb_status_t TDSQL_CALL tdsql_statement_execute(tdsql_statement *statement,
    const tdsql_bindings *bindings, tdsql_response *out, turbodb_error_t *error) {
  turbodb_status_t status = conn_statement_valid(statement, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (!bindings || !out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid prepared execute request");
  if (bindings->struct_size < sizeof(*bindings) || bindings->abi_version != TDSQL_ABI_VERSION ||
      out->struct_size < sizeof(*out) || out->abi_version != TDSQL_ABI_VERSION)
    return conn_error(error, TURBODB_STATUS_ABI_MISMATCH, "incompatible prepared bindings or response ABI");
  if (statement->result) return conn_error(error, TURBODB_STATUS_BUSY, "destroy prepared result before execute");
  if (bindings->count != tdsql_statement_parameters(statement) || (bindings->count && !bindings->values))
    return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "prepared bindings require exact parameter count and values");
  tdsql_connection *b = statement->backend;
  status = conn_command_available(b, error);
  if (status != TURBODB_STATUS_OK) return status;
  tdsql_request request = tdsql_request_default((vstr){vec_data_const(&statement->metadata.sql),vec_size(&statement->metadata.sql)-1});
  request.limits = statement->limits; request.parameter_count = bindings->count;
  request.read_parameter = conn_bound_value; request.parameter_context = bindings->values;
  request.result_context_bytes = bindings->result_context_bytes; request.unique_column_names = bindings->unique_column_names;
  conn_input input = {.prepared=&statement->metadata};
  status = conn_input_parse(b, &request, &request.limits, conn_max_nodes(b), &input, error);
  if (status == TURBODB_STATUS_OK) status = conn_dispatch_input(b, &request, &input, out, error);
  conn_input_discard(&input);
  if (status == TURBODB_STATUS_OK && out->result) {
    statement->result = out->result; out->result->statement = statement;
  }
  return status;
}
turbodb_status_t TDSQL_CALL tdsql_statement_reset(tdsql_statement *statement, turbodb_error_t *error) {
  turbodb_status_t status = conn_statement_valid(statement, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (statement->result) return conn_error(error, TURBODB_STATUS_BUSY, "destroy prepared result before reset");
  return conn_query_available(statement->backend, error);
}
turbodb_status_t TDSQL_CALL tdsql_statement_close(tdsql_statement *statement, turbodb_error_t *error) {
  if (!statement) return TURBODB_STATUS_OK;
  if (statement->result) return conn_error(error, TURBODB_STATUS_BUSY, "destroy prepared result before close");
  tdsql_connection *b = statement->backend;
  const turbodb_status_t status = orm_sql_prepared_close(&statement->metadata, error);
  --b->prepared_count; --b->dependents; b->prepared_bytes -= statement->charged;
  free(statement);
  if (status != TURBODB_STATUS_OK) b->failure = status;
  return status;
}

turbodb_status_t TDSQL_CALL tdsql_connection_state(const tdsql_connection *b,
    tdsql_session_state *out, turbodb_error_t *error) {
  if (!b || !out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL session state request");
  if (out->struct_size < sizeof(*out) || out->abi_version != TDSQL_ABI_VERSION)
    return conn_error(error, TURBODB_STATUS_ABI_MISMATCH, "incompatible TidesSQL session state ABI");
  tdsql_session_state state = tdsql_session_state_default();
  const tdsql_transaction *t = b->sql_transaction ? b->sql_transaction : b->external_transaction;
  state.autocommit = b->autocommit; state.session_read_only = b->session_access == SQLPARSER_READ_ONLY;
  state.in_transaction = t && t->state != REL_FINISHED;
  state.transaction_read_only = state.in_transaction && t->read_only;
  state.rollback_required = state.in_transaction && (t->state != REL_ACTIVE || t->owner.failed);
  state.busy = (b->active && !b->sql_transaction) ||
      (b->sql_transaction && b->sql_transaction->budget.statement_active);
  state.warning_count = b->diagnostics.total; state.failure = b->failure;
  *out = state; return TURBODB_STATUS_OK;
}

static void database_free(tdsql_database *database) {
  tstr_free(database->config.path);
  tstr_free(database->config.family_name);
  free(database);
}
turbodb_status_t TDSQL_CALL tdsql_database_close(tdsql_database *database,
    turbodb_error_t *error) {
  if (!database) return TURBODB_STATUS_OK;
  if (database->connections)
    return conn_error(error, TURBODB_STATUS_BUSY, "close SQL sessions before shared database");
  const int code = database->native ? orm_tidesdb_close(database->native) : ORM_TDB_SUCCESS;
  database_free(database);
  return code == ORM_TDB_SUCCESS ? TURBODB_STATUS_OK : conn_native(error, code, "close");
}
static void conn_dispose(tdsql_connection *b) {
  if (!b) return;
  orm_sql_diagnostics_destroy(&b->diagnostics);
  tdsql_database *shared = b->shared;
  if (shared) --shared->connections;
  /* Legacy connection_close did not expose native close errors. */
  if (b->private_database) (void)tdsql_database_close(shared, NULL);
  free(b);
}

turbodb_status_t TDSQL_CALL tdsql_connection_close(tdsql_connection *b, turbodb_error_t *error) {
  if (!b) return TURBODB_STATUS_OK;
  if (b->dependents > (b->sql_transaction ? 1u : 0u))
    return conn_error(error, TURBODB_STATUS_BUSY, "close relational results statements and transaction handles before connection");
  if (b->sql_transaction) {
    const turbodb_status_t status = conn_sql_end(b, false, error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  conn_dispose(b); return TURBODB_STATUS_OK;
}

turbodb_status_t TDSQL_CALL tdsql_database_connect(tdsql_database *database,
    tdsql_connection **out, turbodb_error_t *error) {
  if (out) *out = NULL;
  if (!database || !out)
    return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid shared SQL session request");
  if (database->connections >= database->config.max_connections)
    return conn_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "shared SQL session capacity exceeded");
  tdsql_connection *b = calloc(1, sizeof(*b));
  if (!b) return conn_error(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate relational backend");
  b->database = database->native; b->family = database->family;
  b->config = database->config;
  b->session_access = SQLPARSER_READ_WRITE; b->autocommit = true;
  turbodb_status_t status = orm_sql_diagnostics_init(&b->diagnostics, b->config.max_warnings, error);
  if (status != TURBODB_STATUS_OK) { conn_dispose(b); return status; }
  b->shared = database; ++database->connections;
  *out = b; return TURBODB_STATUS_OK;
}

static turbodb_status_t database_verify(tdsql_database *database, turbodb_error_t *error) {
  tdsql_connection admission = {.database=database->native, .family=database->family,
      .config=database->config};
  tdsql_transaction *verify = NULL;
  turbodb_status_t status = conn_transaction_new(&admission, &verify, error);
  if (status == TURBODB_STATUS_OK) status = tdsql_transaction_finish(verify, false, error);
  const turbodb_status_t released = tdsql_transaction_release_checked(verify,
      status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? released : status;
}

static turbodb_status_t database_configure(const tdsql_config *config,
    tdsql_database **out, turbodb_error_t *error) {
  if (out) *out = NULL;
  if (!config || !out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational backend request");
  if (config->struct_size < sizeof(*config) || config->abi_version != TDSQL_ABI_VERSION)
    return conn_error(error, TURBODB_STATUS_ABI_MISMATCH, "incompatible TidesSQL config ABI");
  if (config->option_count > UINT32_MAX || (config->option_count && !config->options))
    return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational backend options");
  const tdsql_limits *limits = &config->limits;
  if (!limits->max_parameters || !limits->max_columns || !limits->max_query_bytes ||
      !limits->max_parameter_bytes || !limits->max_result_rows || !limits->max_result_bytes)
    return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "relational limits must be positive");
  tdsql_database *database = calloc(1, sizeof(*database));
  if (!database) return conn_error(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate shared SQL database");
  const turbodb_status_t status = conn_settings(&database->config, config, limits, error);
  if (status != TURBODB_STATUS_OK) { database_free(database); return status; }
  *out = database; return TURBODB_STATUS_OK;
}
static turbodb_status_t database_start(tdsql_database *database, turbodb_error_t *error) {
  turbodb_status_t status = TURBODB_STATUS_OK;
  orm_tidesdb_config_t native = orm_tidesdb_default_config(); native.db_path = database->config.path;
  int code = orm_tidesdb_open(&native, &database->native);
  if (code != ORM_TDB_SUCCESS) { status = conn_native(error, code, "open"); return status; }
  database->family = orm_tidesdb_get_column_family(database->native, database->config.family_name);
  if (database->config.initialize) {
    if (database->family) { status = conn_error(error, TURBODB_STATUS_INVALID_STATE, "sql_initialize requires a new column family"); return status; }
    orm_tidesdb_column_family_config_t family = orm_tidesdb_default_column_family_config(); family.sync_mode = ORM_TDB_SYNC_FULL;
    code = orm_tidesdb_create_column_family(database->native, database->config.family_name, &family);
    if (code != ORM_TDB_SUCCESS) { status = conn_native(error, code, "create relational CF"); return status; }
    database->family = orm_tidesdb_get_column_family(database->native, database->config.family_name);
  }
  if (!database->family) { status = conn_error(error, TURBODB_STATUS_INVALID_STATE, "relational column family does not exist"); return status; }
  if (database->config.initialize) {
    orm_tidesdb_sql_budget budget = {0};
    status = orm_tidesdb_sql_budget_init(&budget, &database->config.budget_limits, error);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_begin(&budget, error);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_catalog_initialize(database->native,
        database->family, database->config.max_record, &budget, error);
    if (budget.statement_active) {
      const turbodb_status_t ended = orm_tidesdb_sql_budget_end(&budget, status == TURBODB_STATUS_OK ? error : NULL);
      if (status == TURBODB_STATUS_OK) status = ended;
    }
    if (status != TURBODB_STATUS_OK) return status;
  }
  return database_verify(database, error);
}

turbodb_status_t TDSQL_CALL tdsql_database_open(const tdsql_config *config,
    tdsql_database **out, turbodb_error_t *error) {
  tdsql_database *database = NULL;
  if (out) *out = NULL;
  if (!out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational backend request");
  turbodb_status_t status = database_configure(config, &database, error);
  if (status != TURBODB_STATUS_OK) return status;
  status = database_start(database, error);
  if (status != TURBODB_STATUS_OK) { (void)tdsql_database_close(database, NULL); return status; }
  *out = database; return conn_error(error, TURBODB_STATUS_OK, NULL);
}

turbodb_status_t TDSQL_CALL tdsql_connection_open(const tdsql_config *config,
    tdsql_connection **out, turbodb_error_t *error) {
  if (out) *out = NULL;
  if (!out) return conn_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid relational backend request");
  tdsql_database *database = NULL;
  tdsql_connection *connection = NULL;
  turbodb_status_t status = database_configure(config, &database, error);
  if (status != TURBODB_STATUS_OK) return status;
  /* Preserve local admission: allocate diagnostics before initializing storage. */
  status = tdsql_database_connect(database, &connection, error);
  if (status == TURBODB_STATUS_OK) status = database_start(database, error);
  if (status != TURBODB_STATUS_OK) {
    conn_dispose(connection); (void)tdsql_database_close(database, NULL); return status;
  }
  connection->database = database->native; connection->family = database->family;
  connection->private_database = true;
  *out = connection; return conn_error(error, TURBODB_STATUS_OK, NULL);
}
