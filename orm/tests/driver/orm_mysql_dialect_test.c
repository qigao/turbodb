#include "dialect.h"

#include <tinytest.h>

#include <string.h>

static orm_limits test_limits(void) {
  orm_limits limits;
  memset(&limits, 0, sizeof(limits));
  limits.max_parameters = 32u;
  limits.max_columns = 32u;
  limits.max_predicates = 32u;
  limits.max_assignments = 32u;
  limits.max_query_bytes = 4096u;
  limits.max_parameter_bytes = 4096u;
  limits.max_result_rows = 1024u;
  limits.max_result_bytes = 65536u;
  return limits;
}

static void expect_sql(
    const mysql_dialect_query_t *query, const char *expected) {
  check_not_null(query->sql);
  check_equal(tstr_len(query->sql), strlen(expected));
  check_equal(memcmp(query->sql, expected, strlen(expected)), 0);
}

spec("mysql provider-local structured dialect") {
  (void)ttest_config__;

  it("renders SELECT with quoted qualified identifiers and ordered binds") {
    orm_limits limits = test_limits();
    orm_query_plan plan;
    mysql_dialect_query_t query;
    orm_error_t error;
    const orm_owned_value *bound;

    orm_error_init(&error);
    check_equal(orm_plan_init(
                    &plan, ORM_QUERY_SELECT,
                    orm_view("app.select"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_column(
                    &plan, orm_view("id"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_column(
                    &plan, orm_view("select"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(
                    &plan, orm_view("id"), ORM_COMPARE_GREATER_EQUAL,
                    orm_i64(7), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(
                    &plan, orm_view("deleted_at"), ORM_COMPARE_EQUAL,
                    orm_null(), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_set_order(
                    &plan, orm_view("select"), ORM_ORDER_DESCENDING,
                    &limits, &error),
                ORM_STATUS_OK);
    plan.offset = 5u;
    plan.has_offset = true;

    check_equal(mysql_dialect_render(
                    &plan, &limits, &query, &error),
                ORM_STATUS_OK);
    expect_sql(
        &query,
        "SELECT `id`, `select` FROM `app`.`select`"
        " WHERE `id` >= ? AND `deleted_at` IS NULL"
        " ORDER BY `select` DESC"
        " LIMIT 18446744073709551615 OFFSET 5");
    check_equal(vec_size(&query.parameters), (size_t)1u);
    bound = *(const orm_owned_value *const *)
        vec_at_const(&query.parameters, 0u);
    check_not_null(bound);
    check_equal(bound->kind, ORM_VALUE_INT64);
    check_equal(bound->data.int64_value, INT64_C(7));

    mysql_dialect_query_destroy(&query);
    orm_plan_destroy(&plan);
  }

  it("renders INSERT with NULL literals and only real binary binds") {
    orm_limits limits = test_limits();
    orm_query_plan plan;
    mysql_dialect_query_t query;
    orm_error_t error;
    const orm_owned_value *bound;

    orm_error_init(&error);
    check_equal(orm_plan_init(
                    &plan, ORM_QUERY_INSERT,
                    orm_view("order"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_assignment(
                    &plan, orm_view("id"), orm_i64(9),
                    &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_assignment(
                    &plan, orm_view("note"), orm_null(),
                    &limits, &error),
                ORM_STATUS_OK);

    check_equal(mysql_dialect_render(
                    &plan, &limits, &query, &error),
                ORM_STATUS_OK);
    expect_sql(
        &query,
        "INSERT INTO `order` (`id`, `note`) VALUES (?, NULL)");
    check_equal(vec_size(&query.parameters), (size_t)1u);
    bound = *(const orm_owned_value *const *)
        vec_at_const(&query.parameters, 0u);
    check_equal(bound->data.int64_value, INT64_C(9));

    mysql_dialect_query_destroy(&query);
    orm_plan_destroy(&plan);
  }

  it("renders UPDATE binds in assignment then predicate order") {
    orm_limits limits = test_limits();
    orm_query_plan plan;
    mysql_dialect_query_t query;
    orm_error_t error;
    const orm_owned_value *first;
    const orm_owned_value *second;

    orm_error_init(&error);
    check_equal(orm_plan_init(
                    &plan, ORM_QUERY_UPDATE,
                    orm_view("people"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_assignment(
                    &plan, orm_view("score"), orm_i64(29),
                    &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(
                    &plan, orm_view("id"), ORM_COMPARE_EQUAL,
                    orm_i64(9001), &limits, &error),
                ORM_STATUS_OK);

    check_equal(mysql_dialect_render(
                    &plan, &limits, &query, &error),
                ORM_STATUS_OK);
    expect_sql(
        &query,
        "UPDATE `people` SET `score` = ? WHERE `id` = ?");
    check_equal(vec_size(&query.parameters), (size_t)2u);

    first = *(const orm_owned_value *const *)
        vec_at_const(&query.parameters, 0u);
    second = *(const orm_owned_value *const *)
        vec_at_const(&query.parameters, 1u);
    check_equal(first->data.int64_value, INT64_C(29));
    check_equal(second->data.int64_value, INT64_C(9001));

    mysql_dialect_query_destroy(&query);
    orm_plan_destroy(&plan);
  }

  it("renders DELETE and rejects NULL range comparisons") {
    orm_limits limits = test_limits();
    orm_query_plan plan;
    mysql_dialect_query_t query;
    orm_error_t error;

    orm_error_init(&error);
    check_equal(orm_plan_init(
                    &plan, ORM_QUERY_DELETE,
                    orm_view("people"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(
                    &plan, orm_view("id"), ORM_COMPARE_EQUAL,
                    orm_i64(4), &limits, &error),
                ORM_STATUS_OK);
    check_equal(mysql_dialect_render(
                    &plan, &limits, &query, &error),
                ORM_STATUS_OK);
    expect_sql(
        &query, "DELETE FROM `people` WHERE `id` = ?");
    mysql_dialect_query_destroy(&query);
    orm_plan_destroy(&plan);

    check_equal(orm_plan_init(
                    &plan, ORM_QUERY_DELETE,
                    orm_view("people"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(
                    &plan, orm_view("id"), ORM_COMPARE_GREATER,
                    orm_null(), &limits, &error),
                ORM_STATUS_OK);
    check_equal(mysql_dialect_render(
                    &plan, &limits, &query, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    orm_plan_destroy(&plan);
  }

  it("rejects RAW plans because RAW lowering has a separate lexer contract") {
    orm_limits limits = test_limits();
    orm_query_plan plan;
    mysql_dialect_query_t query;
    orm_error_t error;

    orm_error_init(&error);
    check_equal(orm_plan_init(
                    &plan, ORM_QUERY_RAW,
                    orm_view("select ?1"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(mysql_dialect_render(
                    &plan, &limits, &query, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    orm_plan_destroy(&plan);
  }
}
