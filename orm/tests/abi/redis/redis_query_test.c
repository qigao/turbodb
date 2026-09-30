#include "orm_internal.h"
#include "query.h"

#include "tinytest.h"

#include <string.h>

static void orm_redis_test_limits(orm_limits *limits) {
  memset(limits, 0, sizeof(*limits));
  limits->max_columns = 8u;
  limits->max_assignments = 8u;
  limits->max_predicates = 8u;
  limits->max_parameters = 16u;
  limits->max_query_bytes = 4096u;
  limits->max_parameter_bytes = 4096u;
  limits->max_result_rows = 50u;
  limits->max_result_bytes = 65536u;
}

static const char *orm_redis_argument(const orm_redis_query *query,
                                      size_t index) {
  return *(const tstr *)vec_at_const(&query->arguments, index);
}

spec("ORM Redis query lowering") {
  it("builds argv with escaped tags, fixed doubles, sort and limit") {
    orm_limits limits;
    orm_query_plan plan;
    orm_redis_query command;
    orm_error_t error;

    orm_redis_test_limits(&limits);
    orm_error_init(&error);
    check_equal(orm_plan_init(&plan, ORM_QUERY_SELECT, orm_view("users"),
                              &limits, &error), ORM_STATUS_OK);
    check_equal(orm_plan_add_column(&plan, orm_view("id"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_column(&plan, orm_view("score"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(&plan, orm_view("tag"),
                                       ORM_COMPARE_EQUAL,
                                       orm_text("a,b"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(&plan, orm_view("score"),
                                       ORM_COMPARE_GREATER,
                                       orm_f64(1.0e20), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_set_order(&plan, orm_view("score"),
                                   ORM_ORDER_DESCENDING, &limits, &error),
                ORM_STATUS_OK);
    plan.has_limit = true;
    plan.limit = 7u;
    plan.has_offset = true;
    plan.offset = 3u;

    check_equal(orm_redis_query_build(&plan, &limits, orm_view("idx:"),
                                      &command, &error), ORM_STATUS_OK);
    check_equal(orm_redis_argument(&command, 0u), "FT.SEARCH");
    check_equal(orm_redis_argument(&command, 1u), "idx:users");
    check_equal(orm_redis_argument(&command, 2u),
                "(@tag:{a\\,b} @score:[(100000000000000000000 +inf])");
    check_equal(orm_redis_argument(&command, 3u), "RETURN");
    check_equal(orm_redis_argument(&command, 4u), "2");
    check_equal(orm_redis_argument(&command, 5u), "id");
    check_equal(orm_redis_argument(&command, 6u), "score");
    check_equal(orm_redis_argument(&command, 7u), "SORTBY");
    check_equal(orm_redis_argument(&command, 8u), "score");
    check_equal(orm_redis_argument(&command, 9u), "DESC");
    check_equal(orm_redis_argument(&command, 10u), "LIMIT");
    check_equal(orm_redis_argument(&command, 11u), "3");
    check_equal(orm_redis_argument(&command, 12u), "7");
    check_equal(orm_redis_argument(&command, 13u), "DIALECT");
    check_equal(orm_redis_argument(&command, 14u), "2");

    orm_redis_query_destroy(&command);
    orm_plan_destroy(&plan);
  }

  it("preserves SQL null exclusion for inequality predicates") {
    orm_limits limits;
    orm_query_plan plan;
    orm_redis_query command;
    orm_error_t error;

    orm_redis_test_limits(&limits);
    orm_error_init(&error);
    check_equal(orm_plan_init(&plan, ORM_QUERY_SELECT, orm_view("users"),
                              &limits, &error), ORM_STATUS_OK);
    check_equal(orm_plan_add_column(&plan, orm_view("id"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(&plan, orm_view("score"),
                                       ORM_COMPARE_NOT_EQUAL, orm_i64(5),
                                       &limits, &error), ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(&plan, orm_view("name"),
                                       ORM_COMPARE_NOT_LIKE,
                                       orm_text("%x_"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_redis_query_build(&plan, &limits, orm_view("idx:"),
                                      &command, &error), ORM_STATUS_OK);
    check_equal(
        orm_redis_argument(&command, 2u),
        "((@score:[-inf +inf] -@score:[5 5]) "
        "(@name:(\"w'*'\") -@name:(\"w'*x?'\")))");
    orm_redis_query_destroy(&command);
    orm_plan_destroy(&plan);
  }
}
