#include "orm_postgres_libpq.h"

#include <limits.h>

_Static_assert(sizeof(Oid) == sizeof(uint32_t),
               "PostgreSQL Oid must match the ORM request type");

static int orm_postgres_libpq_send(
    void *context, const orm_postgres_query_request *request) {
  return PQsendQueryParams(
      (PGconn *)context, request->sql, request->parameter_count,
      (const Oid *)request->parameter_types, request->parameter_values,
      request->parameter_lengths, request->parameter_formats,
      request->result_format);
}

static int orm_postgres_libpq_enable_single_row(void *context) {
  return PQsetSingleRowMode((PGconn *)context);
}

static void *orm_postgres_libpq_next_result(void *context) {
  return PQgetResult((PGconn *)context);
}

static void orm_postgres_libpq_release_result(void *result) {
  PQclear((PGresult *)result);
}

static const char *orm_postgres_libpq_connection_error(void *context) {
  return PQerrorMessage((const PGconn *)context);
}

static orm_postgres_result_status orm_postgres_libpq_status(
    const void *result) {
  switch (PQresultStatus((const PGresult *)result)) {
    case PGRES_SINGLE_TUPLE:
      return ORM_POSTGRES_RESULT_SINGLE_ROW;
    case PGRES_TUPLES_OK:
      return ORM_POSTGRES_RESULT_TUPLES_DONE;
    case PGRES_COMMAND_OK:
      return ORM_POSTGRES_RESULT_COMMAND_DONE;
    default:
      return ORM_POSTGRES_RESULT_ERROR;
  }
}

static int64_t orm_postgres_libpq_rows(const void *result) {
  return (int64_t)PQntuples((const PGresult *)result);
}

static int64_t orm_postgres_libpq_columns(const void *result) {
  return (int64_t)PQnfields((const PGresult *)result);
}

static const char *orm_postgres_libpq_column_name(const void *result,
                                                   size_t column) {
  if (column > (size_t)INT_MAX)
    return NULL;
  return PQfname((const PGresult *)result, (int)column);
}

static uint32_t orm_postgres_libpq_column_type(const void *result,
                                                size_t column) {
  if (column > (size_t)INT_MAX)
    return 0u;
  return (uint32_t)PQftype((const PGresult *)result, (int)column);
}

static int orm_postgres_libpq_is_null(const void *result, size_t row,
                                      size_t column) {
  if (row > (size_t)INT_MAX || column > (size_t)INT_MAX)
    return 1;
  return PQgetisnull((const PGresult *)result, (int)row, (int)column);
}

static const char *orm_postgres_libpq_value(const void *result, size_t row,
                                             size_t column) {
  if (row > (size_t)INT_MAX || column > (size_t)INT_MAX)
    return NULL;
  return PQgetvalue((const PGresult *)result, (int)row, (int)column);
}

static int64_t orm_postgres_libpq_length(const void *result, size_t row,
                                         size_t column) {
  if (row > (size_t)INT_MAX || column > (size_t)INT_MAX)
    return -1;
  return (int64_t)PQgetlength((const PGresult *)result, (int)row,
                              (int)column);
}

static const char *orm_postgres_libpq_command_tuples(const void *result) {
  return PQcmdTuples((PGresult *)result);
}

static const char *orm_postgres_libpq_result_error(const void *result) {
  return PQresultErrorMessage((const PGresult *)result);
}

static const char *orm_postgres_libpq_result_sqlstate(const void *result) {
  return PQresultErrorField((const PGresult *)result, PG_DIAG_SQLSTATE);
}

static const orm_postgres_command_ops orm_postgres_libpq_command_ops = {
    sizeof(orm_postgres_command_ops), ORM_POSTGRES_COMMAND_OPS_ABI_VERSION,
    orm_postgres_libpq_send, orm_postgres_libpq_enable_single_row,
    orm_postgres_libpq_next_result, orm_postgres_libpq_release_result,
    orm_postgres_libpq_connection_error};

static const orm_postgres_result_ops orm_postgres_libpq_result_ops = {
    sizeof(orm_postgres_result_ops), ORM_POSTGRES_RESULT_OPS_ABI_VERSION,
    orm_postgres_libpq_status, orm_postgres_libpq_rows,
    orm_postgres_libpq_columns, orm_postgres_libpq_column_name,
    orm_postgres_libpq_column_type, orm_postgres_libpq_is_null,
    orm_postgres_libpq_value, orm_postgres_libpq_length,
    orm_postgres_libpq_command_tuples, orm_postgres_libpq_result_error,
    orm_postgres_libpq_result_sqlstate};

orm_postgres_driver orm_postgres_libpq_driver(PGconn *connection) {
  orm_postgres_driver driver = {&orm_postgres_libpq_command_ops,
                                &orm_postgres_libpq_result_ops, connection};
  return driver;
}
