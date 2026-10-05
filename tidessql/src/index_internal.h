#ifndef ORM_TIDESDB_SQL_INDEX_INTERNAL_H
#define ORM_TIDESDB_SQL_INDEX_INTERNAL_H
#include "index.h"

/* Derived from immutable parts; never cached separately from the definition. */
static inline bool orm_sql_index_has_double(const orm_sql_index_definition *definition) {
  for (size_t i = 0; i < vec_size(&definition->parts); ++i)
    if (((const orm_sql_index_part *)vec_at_const(&definition->parts, i))->type.kind == TURBODB_VALUE_DOUBLE) return true;
  return false;
}

/* Shared binder/wire invariant; PRIMARY is not a secondary index name. */
static inline bool orm_sql_index_primary_name(vstr name) {
  static const char primary[] = "PRIMARY";
  if (name.len != sizeof(primary) - 1) return false;
  for (size_t i = 0; i < name.len; ++i) {
    char c = name.data[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c != primary[i]) return false;
  }
  return true;
}
#endif
