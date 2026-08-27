#include "orm_internal.h"
#include "orm_sql_render.h"

#include <tinytest.h>

#include <string.h>

static orm_limits orm_sql_test_limits(void) {
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

spec("ORM pure C SQL renderer") {
  it("renders PostgreSQL SELECT and preserves parameter order") {
    orm_limits limits = orm_sql_test_limits();
    orm_query_plan plan;
    orm_sql_query rendered;
    orm_error_t error;
    const orm_owned_value *parameter;

    orm_error_init(&error);
    check_equal(orm_plan_init(&plan, ORM_QUERY_SELECT,
                              vstr_from_cstr("person"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_column(&plan, vstr_from_cstr("id"), &limits,
                                    &error), ORM_STATUS_OK);
    check_equal(orm_plan_add_column(&plan, vstr_from_cstr("name"), &limits,
                                    &error), ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(&plan, vstr_from_cstr("age"),
                                       ORM_COMPARE_GREATER_EQUAL, orm_i64(18),
                                       &limits, &error), ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(&plan, vstr_from_cstr("deleted"),
                                       ORM_COMPARE_EQUAL, orm_null(), &limits,
                                       &error), ORM_STATUS_OK);
    check_equal(orm_plan_set_order(&plan, vstr_from_cstr("name"),
                                   ORM_ORDER_DESCENDING, &limits, &error),
                ORM_STATUS_OK);
    plan.has_limit = true;
    plan.limit = 10u;
    plan.has_offset = true;
    plan.offset = 2u;

    check_equal(orm_sql_render(&plan, &limits, ORM_SQL_POSTGRES, &rendered,
                               &error), ORM_STATUS_OK);
    check_equal(strcmp(rendered.text,
                       "select id, name from person where age >= $1 and "
                       "deleted is null order by name desc limit 10 offset 2"),
                0);
    check_equal(vec_size(&rendered.parameters), (size_t)1u);
    parameter = *(const orm_owned_value *const *)
        vec_at_const(&rendered.parameters, 0u);
    check_not_null(parameter);
    check_equal(parameter->kind, ORM_VALUE_INT64);
    check_equal(parameter->data.int64_value, (int64_t)18);

    orm_sql_query_destroy(&rendered);
    orm_plan_destroy(&plan);
  }

  it("renders SQLite INSERT nulls without consuming placeholders") {
    orm_limits limits = orm_sql_test_limits();
    orm_query_plan plan;
    orm_sql_query rendered;
    orm_error_t error;

    orm_error_init(&error);
    check_equal(orm_plan_init(&plan, ORM_QUERY_INSERT,
                              vstr_from_cstr("person"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_assignment(&plan, vstr_from_cstr("name"),
                                        orm_text("Alice"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_assignment(&plan, vstr_from_cstr("deleted"),
                                        orm_null(), &limits, &error),
                ORM_STATUS_OK);

    check_equal(orm_sql_render(&plan, &limits, ORM_SQL_SQLITE, &rendered,
                               &error), ORM_STATUS_OK);
    check_equal(strcmp(rendered.text,
                       "insert into person (name, deleted) values (?1, null)"),
                0);
    check_equal(vec_size(&rendered.parameters), (size_t)1u);

    orm_sql_query_destroy(&rendered);
    orm_plan_destroy(&plan);
  }

  it("rejects SQL metacharacters in structured identifiers") {
    orm_limits limits = orm_sql_test_limits();
    orm_query_plan plan;
    orm_error_t error;

    orm_error_init(&error);
    check_equal(orm_plan_init(&plan, ORM_QUERY_SELECT,
                              vstr_from_cstr("person;drop_table"), &limits,
                              &error), ORM_STATUS_INVALID_ARGUMENT);
  }
}
