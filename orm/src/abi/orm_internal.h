#ifndef ORM_INTERNAL_H
#define ORM_INTERNAL_H

#include "orm_cbind_source.h"

#include <orm.h>
#include <turbostl/vec.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  ORM_BACKEND_OPS_ABI_VERSION = 1u,
  ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION = 1u
};

typedef enum orm_query_kind {
  ORM_QUERY_SELECT = 0,
  ORM_QUERY_INSERT,
  ORM_QUERY_UPDATE,
  ORM_QUERY_DELETE,
  ORM_QUERY_RAW
} orm_query_kind;

typedef struct orm_limits {
  size_t max_parameters;
  size_t max_columns;
  size_t max_predicates;
  size_t max_assignments;
  size_t max_query_bytes;
  size_t max_parameter_bytes;
  uint64_t max_result_rows;
  uint64_t max_result_bytes;
} orm_limits;

typedef struct orm_owned_value {
  orm_value_kind_t kind;
  union {
    int64_t int64_value;
    uint64_t uint64_value;
    double double_value;
    uint8_t boolean_value;
  } data;
  tstr bytes;
} orm_owned_value;

typedef struct orm_assignment {
  tstr column;
  orm_owned_value value;
} orm_assignment;

typedef struct orm_predicate {
  tstr column;
  orm_compare_t comparison;
  orm_owned_value value;
} orm_predicate;

typedef struct orm_ordering {
  tstr column;
  orm_order_t order;
  bool present;
} orm_ordering;

typedef struct orm_query_plan {
  orm_query_kind kind;
  tstr table;
  tstr raw_sql;
  vec_t columns;
  vec_t assignments;
  vec_t predicates;
  vec_t raw_parameters;
  orm_ordering ordering;
  uint64_t limit;
  uint64_t offset;
  size_t parameter_bytes;
  bool select_all;
  bool has_limit;
  bool has_offset;
} orm_query_plan;

typedef struct orm_backend orm_backend;
typedef struct orm_transaction_backend orm_transaction_backend;

typedef struct orm_transaction_backend_ops {
  size_t struct_size;
  uint32_t abi_version;
  void (*destroy)(void *context);
  orm_status_t (*open_cursor)(void *context, const orm_query_plan *plan,
                              const orm_limits *limits,
                              orm_row_cursor *out_cursor,
                              orm_error_t *error);
  orm_status_t (*execute_command)(void *context, const orm_query_plan *plan,
                                  const orm_limits *limits,
                                  uint64_t *affected_rows,
                                  orm_error_t *error);
  orm_status_t (*commit)(void *context, orm_error_t *error);
  orm_status_t (*rollback)(void *context, orm_error_t *error);
  orm_status_t (*savepoint)(void *context, vstr name, orm_error_t *error);
  orm_status_t (*rollback_to_savepoint)(void *context, vstr name,
                                        orm_error_t *error);
  orm_status_t (*release_savepoint)(void *context, vstr name,
                                    orm_error_t *error);
} orm_transaction_backend_ops;

struct orm_transaction_backend {
  const orm_transaction_backend_ops *ops;
  void *context;
};

typedef struct orm_backend_ops {
  size_t struct_size;
  uint32_t abi_version;
  void (*destroy)(void *context);
  orm_status_t (*open_cursor)(void *context, const orm_query_plan *plan,
                              const orm_limits *limits,
                              orm_row_cursor *out_cursor,
                              orm_error_t *error);
  orm_status_t (*execute_command)(void *context, const orm_query_plan *plan,
                                  const orm_limits *limits,
                                  uint64_t *affected_rows,
                                  orm_error_t *error);
  orm_status_t (*begin_transaction)(void *context, orm_isolation_t isolation,
                                    orm_transaction_backend *out_transaction,
                                    orm_error_t *error);
} orm_backend_ops;

struct orm_backend {
  const orm_backend_ops *ops;
  void *context;
};

struct orm_connection {
  orm_limits limits;
  orm_backend backend;
};

struct orm_query {
  orm_connection_t *connection;
  orm_query_plan plan;
};

typedef enum orm_transaction_state {
  ORM_TRANSACTION_ACTIVE = 0,
  ORM_TRANSACTION_COMMITTED,
  ORM_TRANSACTION_ROLLED_BACK
} orm_transaction_state;

struct orm_transaction {
  orm_connection_t *connection;
  orm_transaction_backend backend;
  orm_transaction_state state;
};

typedef orm_status_t (*orm_backend_factory_v1)(
    const orm_config_t *config, const orm_limits *limits,
    orm_backend *out_backend, orm_error_t *error);

bool orm_query_returns_rows(const orm_query_plan *plan);

ORM_C_API void orm_error_set(orm_error_t *error, orm_status_t status,
                             const char *message);
ORM_C_API bool orm_view_valid(vstr value, bool allow_empty);
ORM_C_API bool orm_view_equal_cstr(vstr value, const char *text);
const orm_option_t *orm_option_find(const orm_config_t *config,
                                    const char *keyword);
ORM_C_API orm_status_t ORM_C_CALL orm_connect_with_factory_v1(
    const orm_config_t *config, orm_backend_factory_v1 factory,
    orm_connection_t **out_connection, orm_error_t *error);

orm_status_t orm_plan_init(orm_query_plan *plan, orm_query_kind kind,
                           vstr input, const orm_limits *limits,
                           orm_error_t *error);
void orm_plan_destroy(orm_query_plan *plan);
orm_status_t orm_plan_select_all(orm_query_plan *plan, orm_error_t *error);
orm_status_t orm_plan_add_column(orm_query_plan *plan, vstr column,
                                 const orm_limits *limits,
                                 orm_error_t *error);
orm_status_t orm_plan_add_assignment(orm_query_plan *plan, vstr column,
                                     orm_value_t value,
                                     const orm_limits *limits,
                                     orm_error_t *error);
orm_status_t orm_plan_add_predicate(orm_query_plan *plan, vstr column,
                                    orm_compare_t comparison,
                                    orm_value_t value,
                                    const orm_limits *limits,
                                    orm_error_t *error);
orm_status_t orm_plan_add_key(orm_query_plan *plan,
                              const orm_key_part_t *parts,
                              uint32_t part_count, const orm_limits *limits,
                              orm_error_t *error);
orm_status_t orm_plan_add_bind(orm_query_plan *plan, orm_value_t value,
                               const orm_limits *limits,
                               orm_error_t *error);
orm_status_t orm_plan_set_order(orm_query_plan *plan, vstr column,
                                orm_order_t order, const orm_limits *limits,
                                orm_error_t *error);

#if defined(ORM_WITH_SQLITE)
orm_status_t orm_sqlite_backend_create(const orm_config_t *config,
                                       const orm_limits *limits,
                                       orm_backend *out_backend,
                                       orm_error_t *error);
#endif
#if defined(ORM_WITH_PGSQL)
orm_status_t orm_postgres_backend_create(const orm_config_t *config,
                                         const orm_limits *limits,
                                         orm_backend *out_backend,
                                         orm_error_t *error);
#endif
#if defined(ORM_WITH_REDIS)
orm_status_t orm_redis_backend_create(const orm_config_t *config,
                                      const orm_limits *limits,
                                      orm_backend *out_backend,
                                      orm_error_t *error);
#endif
#if defined(ORM_WITH_TIDESDB)
orm_status_t orm_tidesdb_backend_create(const orm_config_t *config,
                                        const orm_limits *limits,
                                        orm_backend *out_backend,
                                        orm_error_t *error);
#endif
#if defined(ORM_WITH_MONGO)
orm_status_t orm_mongo_backend_create(const orm_config_t *config,
                                      const orm_limits *limits,
                                      orm_backend *out_backend,
                                      orm_error_t *error);
#endif

#endif
