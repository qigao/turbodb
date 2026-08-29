#include "orm_postgresql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"

static orm_status_t orm_postgres_live_fail(orm_error_t *error,
                                           const char *message) {
  if (error != NULL && error->struct_size >= sizeof(*error)) {
    error->status = ORM_STATUS_INTERNAL_ERROR;
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
  }
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t orm_postgres_live_execute(orm_query_t *query,
                                              orm_transaction_t *transaction,
                                              orm_result_t **out_result,
                                              orm_error_t *error) {
  return transaction == NULL
             ? orm_query_execute(query, out_result, error)
             : orm_query_execute_in_transaction(query, transaction,
                                                out_result, error);
}

static orm_status_t orm_postgres_live_raw(orm_connection_t *connection,
                                          const char *sql,
                                          orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_status_t status = orm_raw(connection, orm_view(sql), &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, error);
  orm_result_destroy(result);
  orm_query_destroy(query);
  return status;
}

static orm_status_t orm_postgres_live_insert(
    orm_connection_t *connection, orm_transaction_t *transaction,
    const char *domain, const char *user, const char *group,
    const char *name, const unsigned char *payload, size_t payload_size,
    int64_t revision, orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_status_t status =
      orm_insert(connection, orm_view("turbodb_orm_live"), &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_set(query, orm_view("domain_id"), orm_text(domain),
                           error);
  if (status == ORM_STATUS_OK)
    status = orm_query_set(query, orm_view("user_id"), orm_text(user), error);
  if (status == ORM_STATUS_OK)
    status =
        orm_query_set(query, orm_view("group_id"), orm_text(group), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_set(query, orm_view("display_name"), orm_text(name),
                           error);
  if (status == ORM_STATUS_OK)
    status = orm_query_set(query, orm_view("payload"),
                           orm_blob(payload, payload_size), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_set(query, orm_view("revision"), orm_i64(revision),
                           error);
  if (status == ORM_STATUS_OK)
    status = orm_postgres_live_execute(query, transaction, &result, error);
  orm_result_destroy(result);
  orm_query_destroy(query);
  return status;
}

static orm_status_t orm_postgres_live_key_rows(
    orm_connection_t *connection, const char *domain, const char *user,
    const char *group, uint64_t expected_rows, orm_error_t *error) {
  const orm_key_part_t key[] = {
      {orm_view("domain_id"), orm_text(domain)},
      {orm_view("user_id"), orm_text(user)},
      {orm_view("group_id"), orm_text(group)}};
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  uint64_t rows = 0u;
  orm_status_t status = orm_query_create(
      connection, orm_view("turbodb_orm_live"), &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_add_column(query, orm_view("revision"), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_where_key(query, key, 3u, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, error);
  if (status == ORM_STATUS_OK)
    status = orm_result_row_count(result, &rows, error);
  if (status == ORM_STATUS_OK && rows != expected_rows)
    status = orm_postgres_live_fail(error,
                                    "PostgreSQL key query row count mismatch");
  orm_result_destroy(result);
  orm_query_destroy(query);
  return status;
}

static orm_status_t orm_postgres_live_run(const char *conninfo,
                                          orm_error_t *error) {
  static const unsigned char expected_payload[] = {0x00u, 0x01u, 0xffu,
                                                    0x7fu};
  const orm_option_t option = {orm_view("conninfo"), orm_view(conninfo)};
  const orm_key_part_t key[] = {
      {orm_view("domain_id"), orm_text("domain-a")},
      {orm_view("user_id"), orm_text("user-a")},
      {orm_view("group_id"), orm_text("group-a")}};
  orm_config_t config;
  orm_connection_t *connection = NULL;
  orm_transaction_t *transaction = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_string_view_t text = {0};
  orm_blob_t blob = {0};
  uint64_t rows = 0u;
  int64_t revision = 0;
  orm_status_t status;

  orm_config(&config);
  config.driver = orm_view("postgresql");
  config.options = &option;
  config.option_count = 1u;
  config.max_result_rows = 1u;

  status = orm_postgresql_connect(&config, &connection, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;
  status = orm_postgres_live_raw(
      connection,
      "create temporary table turbodb_orm_live("
      "domain_id text not null,user_id text not null,"
      "group_id text not null,display_name text not null,"
      "payload bytea not null,revision bigint not null,"
      "primary key(domain_id,user_id,group_id)) on commit preserve rows",
      error);
  if (status != ORM_STATUS_OK)
    goto cleanup;
  status = orm_postgres_live_insert(
      connection, NULL, "domain-a", "user-a", "group-a", "Alice",
      expected_payload, sizeof(expected_payload), 1, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;

  status = orm_query_create(connection, orm_view("turbodb_orm_live"), &query,
                            error);
  if (status == ORM_STATUS_OK)
    status = orm_query_add_column(query, orm_view("display_name"), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_add_column(query, orm_view("payload"), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_add_column(query, orm_view("revision"), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_where_key(query, key, 3u, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, error);
  if (status == ORM_STATUS_OK)
    status = orm_result_row_count(result, &rows, error);
  if (status == ORM_STATUS_OK && rows != 1u)
    status = orm_postgres_live_fail(error,
                                    "PostgreSQL composite key missed its row");
  if (status == ORM_STATUS_OK)
    status = orm_result_get_text(result, 0u, 0u, &text, error);
  if (status == ORM_STATUS_OK &&
      (text.len != 5u || memcmp(text.data, "Alice", 5u) != 0))
    status = orm_postgres_live_fail(error,
                                    "PostgreSQL text round trip mismatch");
  if (status == ORM_STATUS_OK)
    status = orm_result_get_blob(result, 0u, 1u, &blob, error);
  if (status == ORM_STATUS_OK &&
      (blob.size != sizeof(expected_payload) ||
       memcmp(blob.data, expected_payload, sizeof(expected_payload)) != 0))
    status = orm_postgres_live_fail(error,
                                    "PostgreSQL bytea round trip mismatch");
  if (status == ORM_STATUS_OK)
    status = orm_result_get_int64(result, 0u, 2u, &revision, error);
  if (status == ORM_STATUS_OK && revision != 1)
    status = orm_postgres_live_fail(error,
                                    "PostgreSQL integer round trip mismatch");
  orm_result_destroy(result);
  orm_query_destroy(query);
  result = NULL;
  query = NULL;
  if (status != ORM_STATUS_OK)
    goto cleanup;

  status = orm_postgres_live_insert(
      connection, NULL, "domain-a", "user-a", "group-a", "duplicate",
      expected_payload, sizeof(expected_payload), 2, error);
  if (status != ORM_STATUS_CONSTRAINT) {
    if (status == ORM_STATUS_OK)
      status = orm_postgres_live_fail(
          error, "PostgreSQL duplicate key did not map to constraint");
    goto cleanup;
  }
  status = ORM_STATUS_OK;

  status = orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                 &transaction, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;
  status = orm_postgres_live_insert(
      connection, transaction, "domain-a", "user-commit", "group-a",
      "Committed", expected_payload, sizeof(expected_payload), 3, error);
  if (status == ORM_STATUS_OK)
    status = orm_transaction_commit(transaction, error);
  orm_transaction_destroy(transaction);
  transaction = NULL;
  if (status != ORM_STATUS_OK)
    goto cleanup;

  status = orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                 &transaction, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;
  status = orm_postgres_live_insert(
      connection, transaction, "domain-a", "user-rollback", "group-a",
      "Rolled back", expected_payload, sizeof(expected_payload), 4, error);
  if (status == ORM_STATUS_OK)
    status = orm_transaction_rollback(transaction, error);
  orm_transaction_destroy(transaction);
  transaction = NULL;
  if (status != ORM_STATUS_OK)
    goto cleanup;

  status = orm_postgres_live_key_rows(connection, "domain-a", "user-commit",
                                      "group-a", 1u, error);
  if (status == ORM_STATUS_OK)
    status = orm_postgres_live_key_rows(connection, "domain-a",
                                        "user-rollback", "group-a", 0u,
                                        error);
  if (status != ORM_STATUS_OK)
    goto cleanup;

  status = orm_query_create(connection, orm_view("turbodb_orm_live"), &query,
                            error);
  if (status == ORM_STATUS_OK)
    status = orm_query_select_all(query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, error);
  if (status != ORM_STATUS_LIMIT_EXCEEDED) {
    if (status == ORM_STATUS_OK)
      status = orm_postgres_live_fail(
          error, "PostgreSQL result row limit was not enforced");
    goto cleanup;
  }
  status = ORM_STATUS_OK;

cleanup:
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_transaction_destroy(transaction);
  orm_disconnect(connection);
  return status;
}

spec("ORM PostgreSQL live component") {
  it("round trips values, transactions, constraints, keys, and limits") {
    const char *conninfo = getenv("TURBODB_ORM_PGSQL_TEST_CONNINFO");
    orm_error_t error;
    orm_status_t status;

    orm_error_init(&error);
    check_not_null(conninfo);
    if (conninfo == NULL)
      return;
    status = orm_postgres_live_run(conninfo, &error);
    check_equal(status, ORM_STATUS_OK);
  }
}
