#include "orm_internal.h"
#include "orm_mysql_sql.h"

#include <tinytest.h>

#include <string.h>

static orm_limits mysql_sql_limits(void) {
  orm_limits limits = {0};
  limits.max_parameters = 16u;
  limits.max_columns = 16u;
  limits.max_predicates = 16u;
  limits.max_assignments = 16u;
  limits.max_query_bytes = 1024u;
  limits.max_parameter_bytes = 1024u;
  limits.max_result_rows = 128u;
  limits.max_result_bytes = 4096u;
  return limits;
}

spec("MySQL Driver-local SQL projection") {
  it("lowers structured parameters to positional placeholders") {
    orm_limits limits = mysql_sql_limits();
    orm_query_plan plan;
    orm_sql_query rendered;
    orm_error_t error;
    const orm_owned_value *first;
    const orm_owned_value *second;

    orm_error_init(&error);
    check_equal(orm_plan_init(&plan, ORM_QUERY_INSERT,
                              vstr_from_cstr("person"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_assignment(&plan, vstr_from_cstr("name"),
                                        orm_text("Alice"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_assignment(&plan, vstr_from_cstr("score"),
                                        orm_i64(17), &limits, &error),
                ORM_STATUS_OK);

    check_equal(orm_mysql_sql_render(&plan, &limits, &rendered, &error),
                ORM_STATUS_OK);
    check_equal(strcmp(rendered.text,
                       "insert into person (name, score) values (?, ?)"), 0);
    check_equal(vec_size(&rendered.parameters), (size_t)2u);
    first = *(const orm_owned_value *const *)
        vec_at_const(&rendered.parameters, 0u);
    second = *(const orm_owned_value *const *)
        vec_at_const(&rendered.parameters, 1u);
    check_equal(first->kind, ORM_VALUE_TEXT);
    check_equal(tstr_len(first->bytes), (size_t)5u);
    check_equal(memcmp(first->bytes, "Alice", 5u), 0);
    check_equal(second->kind, ORM_VALUE_INT64);
    check_equal(second->data.int64_value, INT64_C(17));

    orm_sql_query_destroy(&rendered);
    orm_plan_destroy(&plan);
  }

  it("reorders repeated portable raw binds without touching literals or comments") {
    orm_limits limits = mysql_sql_limits();
    orm_query_plan plan;
    orm_sql_query rendered;
    orm_error_t error;
    const orm_owned_value *parameter;

    orm_error_init(&error);
    check_equal(orm_plan_init(
                    &plan, ORM_QUERY_RAW,
                    vstr_from_cstr(
                        "select ?2, ?1, ?2, '?1', \"?1\", `?1`, # ?1\n"
                        "/* ?1 */ ?1"),
                    &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_bind(&plan, orm_i64(11), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_bind(&plan, orm_u64(22), &limits, &error),
                ORM_STATUS_OK);

    check_equal(orm_mysql_sql_render(&plan, &limits, &rendered, &error),
                ORM_STATUS_OK);
    check_equal(strcmp(
                    rendered.text,
                    "select ?, ?, ?, '?1', \"?1\", `?1`, # ?1\n"
                    "/* ?1 */ ?"),
                0);
    check_equal(vec_size(&rendered.parameters), (size_t)4u);

    parameter = *(const orm_owned_value *const *)
        vec_at_const(&rendered.parameters, 0u);
    check_equal(parameter->kind, ORM_VALUE_UINT64);
    check_equal(parameter->data.uint64_value, UINT64_C(22));
    parameter = *(const orm_owned_value *const *)
        vec_at_const(&rendered.parameters, 1u);
    check_equal(parameter->kind, ORM_VALUE_INT64);
    check_equal(parameter->data.int64_value, INT64_C(11));
    parameter = *(const orm_owned_value *const *)
        vec_at_const(&rendered.parameters, 2u);
    check_equal(parameter->kind, ORM_VALUE_UINT64);
    check_equal(parameter->data.uint64_value, UINT64_C(22));
    parameter = *(const orm_owned_value *const *)
        vec_at_const(&rendered.parameters, 3u);
    check_equal(parameter->kind, ORM_VALUE_INT64);
    check_equal(parameter->data.int64_value, INT64_C(11));

    orm_sql_query_destroy(&rendered);
    orm_plan_destroy(&plan);
  }

  it("accepts native positional placeholders in original bind order") {
    orm_limits limits = mysql_sql_limits();
    orm_query_plan plan;
    orm_sql_query rendered;
    orm_error_t error;
    const orm_owned_value *parameter;

    orm_error_init(&error);
    check_equal(orm_plan_init(&plan, ORM_QUERY_RAW,
                              vstr_from_cstr("select ?, ?, ?"),
                              &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_bind(&plan, orm_i64(3), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_bind(&plan, orm_i64(5), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_bind(&plan, orm_i64(8), &limits, &error),
                ORM_STATUS_OK);

    check_equal(orm_mysql_sql_render(&plan, &limits, &rendered, &error),
                ORM_STATUS_OK);
    check_equal(strcmp(rendered.text, "select ?, ?, ?"), 0);
    check_equal(vec_size(&rendered.parameters), (size_t)3u);
    parameter = *(const orm_owned_value *const *)
        vec_at_const(&rendered.parameters, 0u);
    check_equal(parameter->data.int64_value, INT64_C(3));
    parameter = *(const orm_owned_value *const *)
        vec_at_const(&rendered.parameters, 1u);
    check_equal(parameter->data.int64_value, INT64_C(5));
    parameter = *(const orm_owned_value *const *)
        vec_at_const(&rendered.parameters, 2u);
    check_equal(parameter->data.int64_value, INT64_C(8));

    orm_sql_query_destroy(&rendered);
    orm_plan_destroy(&plan);
  }

  it("rejects mixed, missing, and unused raw bindings") {
    orm_limits limits = mysql_sql_limits();
    orm_query_plan plan;
    orm_sql_query rendered;
    orm_error_t error;

    orm_error_init(&error);
    check_equal(orm_plan_init(&plan, ORM_QUERY_RAW,
                              vstr_from_cstr("select ?1, ?"),
                              &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_bind(&plan, orm_i64(1), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_bind(&plan, orm_i64(2), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_mysql_sql_render(&plan, &limits, &rendered, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    orm_plan_destroy(&plan);

    check_equal(orm_plan_init(&plan, ORM_QUERY_RAW,
                              vstr_from_cstr("select ?2"),
                              &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_bind(&plan, orm_i64(1), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_mysql_sql_render(&plan, &limits, &rendered, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    orm_plan_destroy(&plan);

    check_equal(orm_plan_init(&plan, ORM_QUERY_RAW,
                              vstr_from_cstr("select ?1"),
                              &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_bind(&plan, orm_i64(1), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_bind(&plan, orm_i64(2), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_mysql_sql_render(&plan, &limits, &rendered, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    orm_plan_destroy(&plan);
  }

  it("uses the MySQL no-limit sentinel for offset-only SELECT") {
    orm_limits limits = mysql_sql_limits();
    orm_query_plan plan;
    orm_sql_query rendered;
    orm_error_t error;

    orm_error_init(&error);
    check_equal(orm_plan_init(&plan, ORM_QUERY_SELECT,
                              vstr_from_cstr("person"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_select_all(&plan, &error), ORM_STATUS_OK);
    plan.has_offset = true;
    plan.offset = 9u;

    check_equal(orm_mysql_sql_render(&plan, &limits, &rendered, &error),
                ORM_STATUS_OK);
    check_equal(strcmp(
                    rendered.text,
                    "select * from person limit 18446744073709551615 offset 9"),
                0);

    orm_sql_query_destroy(&rendered);
    orm_plan_destroy(&plan);
  }
}
