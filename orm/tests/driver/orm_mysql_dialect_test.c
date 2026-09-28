#include "orm_internal.h"
#include "dialect.h"

#include <tinytest.h>

#include <string.h>

static orm_limits mysql_dialect_limits(void) {
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

spec("MySQL local structured dialect") {
  it("renders SELECT with native positional markers and MySQL offset syntax") {
    orm_limits limits = mysql_dialect_limits();
    orm_query_plan plan;
    orm_mysql_rendered_query rendered;
    orm_error_t error;

    orm_error_init(&error);
    check_equal(
        orm_plan_init(
            &plan, ORM_QUERY_SELECT,
            vstr_from_cstr("people"), &limits, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_plan_add_column(
            &plan, vstr_from_cstr("id"), &limits, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_plan_add_column(
            &plan, vstr_from_cstr("score"), &limits, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_plan_add_predicate(
            &plan, vstr_from_cstr("score"),
            ORM_COMPARE_GREATER_EQUAL, orm_i64(10),
            &limits, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_plan_add_predicate(
            &plan, vstr_from_cstr("deleted"),
            ORM_COMPARE_EQUAL, orm_null(),
            &limits, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_plan_set_order(
            &plan, vstr_from_cstr("id"),
            ORM_ORDER_DESCENDING, &limits, &error),
        ORM_STATUS_OK);
    plan.has_offset = true;
    plan.offset = 3u;

    memset(&rendered, 0, sizeof(rendered));
    check_equal(
        orm_mysql_render_plan(
            &plan, &limits, &rendered, &error),
        ORM_STATUS_OK);
    check_equal(
        strcmp(
            rendered.text,
            "select id, score from people where score >= ? and "
            "deleted is null order by id desc "
            "limit 18446744073709551615 offset 3"),
        0);
    check_equal(rendered.parameter_count, (size_t)1u);
    check_not_null(rendered.parameters[0]);
    check_equal(
        rendered.parameters[0]->kind, ORM_VALUE_INT64);
    check_equal(
        rendered.parameters[0]->data.int64_value,
        INT64_C(10));

    orm_mysql_rendered_query_destroy(&rendered);
    orm_plan_destroy(&plan);
  }

  it("renders INSERT and omits null from binary parameter order") {
    orm_limits limits = mysql_dialect_limits();
    orm_query_plan plan;
    orm_mysql_rendered_query rendered;
    orm_error_t error;

    orm_error_init(&error);
    check_equal(
        orm_plan_init(
            &plan, ORM_QUERY_INSERT,
            vstr_from_cstr("people"), &limits, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_plan_add_assignment(
            &plan, vstr_from_cstr("id"), orm_i64(7),
            &limits, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_plan_add_assignment(
            &plan, vstr_from_cstr("name"), orm_text("Alice"),
            &limits, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_plan_add_assignment(
            &plan, vstr_from_cstr("deleted"), orm_null(),
            &limits, &error),
        ORM_STATUS_OK);

    memset(&rendered, 0, sizeof(rendered));
    check_equal(
        orm_mysql_render_plan(
            &plan, &limits, &rendered, &error),
        ORM_STATUS_OK);
    check_equal(
        strcmp(
            rendered.text,
            "insert into people (id, name, deleted) "
            "values (?, ?, null)"),
        0);
    check_equal(rendered.parameter_count, (size_t)2u);
    check_equal(
        rendered.parameters[0]->data.int64_value,
        INT64_C(7));
    check_equal(
        strcmp(rendered.parameters[1]->bytes, "Alice"),
        0);

    orm_mysql_rendered_query_destroy(&rendered);
    orm_plan_destroy(&plan);
  }

  it("preserves UPDATE assignment then predicate parameter order") {
    orm_limits limits = mysql_dialect_limits();
    orm_query_plan plan;
    orm_mysql_rendered_query rendered;
    orm_error_t error;

    orm_error_init(&error);
    check_equal(
        orm_plan_init(
            &plan, ORM_QUERY_UPDATE,
            vstr_from_cstr("people"), &limits, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_plan_add_assignment(
            &plan, vstr_from_cstr("score"), orm_i64(19),
            &limits, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_plan_add_assignment(
            &plan, vstr_from_cstr("name"), orm_text("beta"),
            &limits, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_plan_add_predicate(
            &plan, vstr_from_cstr("id"),
            ORM_COMPARE_EQUAL, orm_i64(7),
            &limits, &error),
        ORM_STATUS_OK);

    memset(&rendered, 0, sizeof(rendered));
    check_equal(
        orm_mysql_render_plan(
            &plan, &limits, &rendered, &error),
        ORM_STATUS_OK);
    check_equal(
        strcmp(
            rendered.text,
            "update people set score = ?, name = ? where id = ?"),
        0);
    check_equal(rendered.parameter_count, (size_t)3u);
    check_equal(
        rendered.parameters[0]->data.int64_value,
        INT64_C(19));
    check_equal(
        strcmp(rendered.parameters[1]->bytes, "beta"),
        0);
    check_equal(
        rendered.parameters[2]->data.int64_value,
        INT64_C(7));

    orm_mysql_rendered_query_destroy(&rendered);
    orm_plan_destroy(&plan);
  }

  it("renders DELETE predicates through prepared parameters") {
    orm_limits limits = mysql_dialect_limits();
    orm_query_plan plan;
    orm_mysql_rendered_query rendered;
    orm_error_t error;

    orm_error_init(&error);
    check_equal(
        orm_plan_init(
            &plan, ORM_QUERY_DELETE,
            vstr_from_cstr("people"), &limits, &error),
        ORM_STATUS_OK);
    check_equal(
        orm_plan_add_predicate(
            &plan, vstr_from_cstr("id"),
            ORM_COMPARE_EQUAL, orm_i64(9),
            &limits, &error),
        ORM_STATUS_OK);

    memset(&rendered, 0, sizeof(rendered));
    check_equal(
        orm_mysql_render_plan(
            &plan, &limits, &rendered, &error),
        ORM_STATUS_OK);
    check_equal(
        strcmp(rendered.text, "delete from people where id = ?"),
        0);
    check_equal(rendered.parameter_count, (size_t)1u);
    check_equal(
        rendered.parameters[0]->data.int64_value,
        INT64_C(9));

    orm_mysql_rendered_query_destroy(&rendered);
    orm_plan_destroy(&plan);
  }
}
