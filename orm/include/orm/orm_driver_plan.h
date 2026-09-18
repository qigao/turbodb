#ifndef ORM_DRIVER_PLAN_H
#define ORM_DRIVER_PLAN_H

#include "orm_driver_base.h"
#include "orm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Candidate SDK: public types only; implementations belong to the host. */
#define ORM_DRIVER_PLAN_SELECT UINT32_C(0)
#define ORM_DRIVER_PLAN_INSERT UINT32_C(1)
#define ORM_DRIVER_PLAN_UPDATE UINT32_C(2)
#define ORM_DRIVER_PLAN_DELETE UINT32_C(3)
#define ORM_DRIVER_PLAN_RAW_SQL UINT32_C(4)
#define ORM_DRIVER_PLAN_SELECT_ALL (UINT32_C(1) << 0)
#define ORM_DRIVER_PLAN_HAS_LIMIT (UINT32_C(1) << 1)
#define ORM_DRIVER_PLAN_HAS_OFFSET (UINT32_C(1) << 2)

typedef struct orm_driver_limits_v1 {
  orm_driver_header_v1 header;
  uint64_t max_parameters;
  uint64_t max_columns;
  uint64_t max_predicates;
  uint64_t max_assignments;
  uint64_t max_query_bytes;
  uint64_t max_parameter_bytes;
  uint64_t max_result_rows;
  uint64_t max_result_bytes;
} orm_driver_limits_v1;

typedef struct orm_driver_value_v1 {
  orm_driver_header_v1 header;
  /* Explicitly mapped to ORM_VALUE_* by the host; unknown kinds are errors. */
  uint32_t kind;
  uint32_t reserved;
  union {
    int64_t sint;
    uint64_t uint;
    double real;
    uint8_t boolean;
    orm_driver_bytes_v1 bytes;
  } data;
} orm_driver_value_v1;

typedef struct orm_driver_plan_meta_v1 {
  orm_driver_header_v1 header;
  uint32_t kind;
  uint32_t flags;
  orm_driver_bytes_v1 table;
  orm_driver_bytes_v1 raw_sql;
  uint64_t column_count;
  uint64_t assignment_count;
  uint64_t predicate_count;
  uint64_t raw_parameter_count;
  uint64_t limit;
  uint64_t offset;
} orm_driver_plan_meta_v1;

typedef struct orm_driver_assignment_v1 {
  orm_driver_header_v1 header;
  orm_driver_bytes_v1 column;
  orm_driver_value_v1 value;
} orm_driver_assignment_v1;

typedef struct orm_driver_predicate_v1 {
  orm_driver_header_v1 header;
  orm_driver_bytes_v1 column;
  uint32_t comparison;
  uint32_t reserved;
  orm_driver_value_v1 value;
} orm_driver_predicate_v1;

typedef struct orm_driver_ordering_v1 {
  orm_driver_header_v1 header;
  orm_driver_bytes_v1 column;
  uint32_t present;
  uint32_t order;
} orm_driver_ordering_v1;

typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_describe_fn)(
    const void *, orm_driver_plan_meta_v1 *, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_column_fn)(
    const void *, uint64_t, orm_driver_bytes_v1 *, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_ordering_fn)(
    const void *, orm_driver_ordering_v1 *, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_assignment_fn)(
    const void *, uint64_t, orm_driver_assignment_v1 *, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_predicate_fn)(
    const void *, uint64_t, orm_driver_predicate_v1 *, orm_error_t *);
typedef orm_status_t (ORM_DRIVER_CALL *orm_driver_parameter_fn)(
    const void *, uint64_t, orm_driver_value_v1 *, orm_error_t *);

typedef struct orm_driver_plan_metadata_ops_v1 {
  orm_driver_header_v1 header;
  orm_driver_describe_fn describe;
  orm_driver_column_fn column_at;
  orm_driver_ordering_fn ordering;
} orm_driver_plan_metadata_ops_v1;

typedef struct orm_driver_plan_value_ops_v1 {
  orm_driver_header_v1 header;
  orm_driver_assignment_fn assignment_at;
  orm_driver_predicate_fn predicate_at;
  orm_driver_parameter_fn raw_parameter_at;
} orm_driver_plan_value_ops_v1;

typedef struct orm_driver_plan_view_v1 {
  orm_driver_header_v1 header;
  const void *context;
  orm_driver_table_v1 metadata;
  orm_driver_table_v1 values;
} orm_driver_plan_view_v1;

/* Accessor indices are zero-based. Views borrow a frozen host plan for the
 * entire execution lease. This DTO alone does not acquire that lease (#28). */
#ifdef __cplusplus
}
#endif
#endif
