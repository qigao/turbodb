#include "orm_internal.h"
#include "query.h"

#include <tinytest.h>

#include <bson/bson.h>

#include <string.h>

static orm_limits orm_mongo_query_limits(void) {
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

spec("ORM pure C MongoDB query translation") {
  it("maps the configured id and translates LIKE without SQL execution") {
    orm_limits limits = orm_mongo_query_limits();
    orm_mongo_settings settings = {
        NULL, NULL, tstr_dup("id")};
    orm_query_plan plan;
    orm_error_t error;
    bson_t filter = BSON_INITIALIZER;
    bson_t options = BSON_INITIALIZER;
    char *filter_json;
    char *options_json;

    orm_error_init(&error);
    check_not_null(settings.id_column);
    check_equal(orm_plan_init(&plan, ORM_QUERY_SELECT,
                              vstr_from_cstr("person"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_column(&plan, vstr_from_cstr("id"), &limits,
                                    &error), ORM_STATUS_OK);
    check_equal(orm_plan_add_column(&plan, vstr_from_cstr("name"), &limits,
                                    &error), ORM_STATUS_OK);
    check_equal(orm_plan_add_predicate(&plan, vstr_from_cstr("name"),
                                       ORM_COMPARE_LIKE, orm_text("A_%"),
                                       &limits, &error), ORM_STATUS_OK);
    check_equal(orm_mongo_append_filter(&filter, &plan, &settings, &limits,
                                        &error), ORM_STATUS_OK);
    check_equal(orm_mongo_append_find_options(&options, &plan, &settings,
                                              &error), ORM_STATUS_OK);

    filter_json = bson_as_canonical_extended_json(&filter, NULL);
    options_json = bson_as_canonical_extended_json(&options, NULL);
    check_not_null(filter_json);
    check_not_null(options_json);
    check_not_null(strstr(filter_json, "\\\\A(?:A..*)\\\\z"));
    check_not_null(strstr(options_json, "\"_id\""));
    check_not_null(strstr(options_json, "\"name\""));

    bson_free(options_json);
    bson_free(filter_json);
    bson_destroy(&options);
    bson_destroy(&filter);
    orm_plan_destroy(&plan);
    tstr_free(settings.id_column);
  }

  it("stores the logical id in MongoDB's reserved native field") {
    orm_limits limits = orm_mongo_query_limits();
    orm_mongo_settings settings = {
        NULL, NULL, tstr_dup("id")};
    orm_query_plan plan;
    orm_error_t error;
    bson_t document = BSON_INITIALIZER;
    bson_iter_t id;

    orm_error_init(&error);
    check_not_null(settings.id_column);
    check_equal(orm_plan_init(&plan, ORM_QUERY_INSERT,
                              vstr_from_cstr("person"), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_plan_add_assignment(&plan, vstr_from_cstr("id"),
                                        orm_i64(7), &limits, &error),
                ORM_STATUS_OK);
    check_equal(orm_mongo_append_insert_document(&document, &plan, &settings,
                                                 &error), ORM_STATUS_OK);
    check_true(bson_iter_init_find(&id, &document, "_id"));
    check_equal(bson_iter_int64(&id), (int64_t)7);

    bson_destroy(&document);
    orm_plan_destroy(&plan);
    tstr_free(settings.id_column);
  }
}
