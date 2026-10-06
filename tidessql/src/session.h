#ifndef ORM_TIDESDB_SQL_SESSION_H
#define ORM_TIDESDB_SQL_SESSION_H
#include "value.h"
#include <sqlparser/sqlparser.h>

typedef enum orm_sql_session_variable {
  ORM_SQL_SESSION_AUTOCOMMIT, ORM_SQL_SESSION_TRANSACTION_ISOLATION,
  ORM_SQL_SESSION_TRANSACTION_READ_ONLY, ORM_SQL_SESSION_WRITABLE_COUNT,
  ORM_SQL_SESSION_AUTO_INCREMENT_INCREMENT=ORM_SQL_SESSION_WRITABLE_COUNT,
  ORM_SQL_SESSION_CHARACTER_SET_CLIENT, ORM_SQL_SESSION_CHARACTER_SET_CONNECTION,
  ORM_SQL_SESSION_CHARACTER_SET_RESULTS, ORM_SQL_SESSION_CHARACTER_SET_SERVER,
  ORM_SQL_SESSION_COLLATION_SERVER, ORM_SQL_SESSION_COLLATION_CONNECTION,
  ORM_SQL_SESSION_INIT_CONNECT, ORM_SQL_SESSION_INTERACTIVE_TIMEOUT,
  ORM_SQL_SESSION_LICENSE, ORM_SQL_SESSION_LOWER_CASE_TABLE_NAMES,
  ORM_SQL_SESSION_MAX_ALLOWED_PACKET, ORM_SQL_SESSION_NET_WRITE_TIMEOUT,
  ORM_SQL_SESSION_PERFORMANCE_SCHEMA, ORM_SQL_SESSION_QUERY_CACHE_SIZE,
  ORM_SQL_SESSION_QUERY_CACHE_TYPE, ORM_SQL_SESSION_SQL_MODE,
  ORM_SQL_SESSION_SYSTEM_TIME_ZONE, ORM_SQL_SESSION_TIME_ZONE,
  ORM_SQL_SESSION_WAIT_TIMEOUT, ORM_SQL_SESSION_VARIABLE_COUNT
} orm_sql_session_variable;
/* Derived once from the connection at statement admission, copied by value.
 * read_only is the session default, not the active or next transaction mode.
 * Single synchronous owner; no pointers, mutable variable table or I/O. */
typedef struct orm_sql_session_snapshot {
  bool valid, autocommit, read_only;
  uint64_t max_allowed_packet;
} orm_sql_session_snapshot;
/* Bounded ASCII @@name / @@SESSION.name / @@LOCAL.name resolution. Charges the
 * span before scanning; borrowed document lasts only this call. Unknown names,
 * user variables and other scopes return UNSUPPORTED. Failure preserves out. */
turbodb_status_t orm_sql_session_resolve(const sqlparser_document *document,
    const sqlparser_node *node, orm_tidesdb_sql_budget *budget,
    orm_sql_session_variable *out, turbodb_error_t *error);
/* IDs must be within VARIABLE_COUNT. Names and TEXT values have static lifetime.
 * No allocation/state change. Missing context -> UNSUPPORTED; invalid ID/output
 * -> INVALID_ARGUMENT. display selects SHOW's ON/OFF instead of I64 0/1.
 * Example: session_value(snapshot, AUTOCOMMIT, false, &value, error). */
vstr orm_sql_session_name(orm_sql_session_variable variable);
orm_sql_type orm_sql_session_type(orm_sql_session_variable variable);
/* ASCII lookup for readable variables; borrows name for this call only.
 * Unknown name returns false without changing out. NULL out is invalid. */
bool orm_sql_session_find(vstr name, orm_sql_session_variable *out);
/* SET accepts only the mutable prefix ending at WRITABLE_COUNT. */
bool orm_sql_session_find_writable(vstr name, orm_sql_session_variable *out);
turbodb_status_t orm_sql_session_value(orm_sql_session_snapshot snapshot,
    orm_sql_session_variable variable, bool display, turbodb_value_t *out, turbodb_error_t *error);
#endif
