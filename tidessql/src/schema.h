#ifndef ORM_TIDESDB_SQL_SCHEMA_H
#define ORM_TIDESDB_SQL_SCHEMA_H
#include "value.h"

enum { ORM_SQL_SELECT_NAME_BYTES = 63 };
typedef struct orm_sql_schema_column {
  vstr name;
  orm_sql_type type;
  vstr qualifier; /* Optional per-column relation alias in a composed scope. */
  size_t lexical_depth; /* Zero for a local schema; nearest outer frame first. */
  bool *capture_used; /* Binding-only flag owned by this query's frame. */
  bool qualified_only; /* Redundant USING column: raw qualified access remains. */
  size_t star_order; /* One-based visible FROM order; zero outside FROM binding. */
} orm_sql_schema_column;
typedef struct orm_sql_column_default {
  turbodb_value_t value;
  bool specified;
} orm_sql_column_default;
/* Borrowed, immutable schema. Consumers must copy it before retaining it. */
typedef struct orm_sql_table_schema {
  vstr name;
  const orm_sql_schema_column *columns;
  size_t count;
  /* Optional table-definition metadata parallel to columns. Query-derived
   * schemas leave this NULL because expressions do not own DDL defaults. */
  const orm_sql_column_default *defaults;
} orm_sql_table_schema;
#endif
