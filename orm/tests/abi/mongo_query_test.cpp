#include "query.hpp"

#include <bson/bson.h>
#include <tinytest.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace orm_c_detail;

mongo_settings settings()
{
    mongo_settings value;
    value.uri = "mongodb://127.0.0.1:27017";
    value.database = "testdb";
    value.id_column = "id";
    return value;
}

struct plan_fixture {
    query_kind kind = query_kind::select;
    std::string table = "person";
    std::string raw_sql;
    std::vector<std::string> columns;
    std::vector<aggregate_expression> aggregates;
    std::vector<assignment> assignments;
    std::vector<join_clause> joins;
    std::vector<std::string> group_columns;
    condition_node where_root;
    condition_node having_root;
    std::vector<bound_parameter> raw_parameters;
    std::optional<ordering_spec> ordering;
    std::optional<std::uint64_t> limit;
    std::optional<std::uint64_t> offset;
    std::size_t parameter_count = 0;
    bool select_all = false;

    query_plan view() const
    {
        return query_plan{kind, table, raw_sql, columns, aggregates, assignments,
                          joins, group_columns, where_root, having_root,
                          raw_parameters, ordering, limit, offset,
                          parameter_count, select_all};
    }

    void add_predicate(std::string column,
                       orm_compare_t comparison,
                       bound_parameter parameter)
    {
        auto node = std::make_unique<condition_node>();
        node->is_group = false;
        node->value.column = std::move(column);
        node->value.comparison = comparison;
        node->value.parameter = std::move(parameter);
        where_root.children.push_back(std::move(node));
        ++parameter_count;
    }

    void add_null_predicate(std::string column, orm_compare_t comparison)
    {
        auto node = std::make_unique<condition_node>();
        node->is_group = false;
        node->value.column = std::move(column);
        node->value.comparison = comparison;
        node->value.has_parameter = false;
        where_root.children.push_back(std::move(node));
    }
};

bound_parameter text_value(std::string value)
{
    bound_parameter parameter;
    parameter.kind = ORM_VALUE_TEXT;
    parameter.text = std::move(value);
    return parameter;
}

bound_parameter integer_value(std::int64_t value)
{
    bound_parameter parameter;
    parameter.kind = ORM_VALUE_INT64;
    parameter.int64_value = value;
    parameter.text = std::to_string(value);
    return parameter;
}

bound_parameter double_value(double value)
{
    bound_parameter parameter;
    parameter.kind = ORM_VALUE_DOUBLE;
    parameter.double_value = value;
    parameter.text = std::to_string(value);
    return parameter;
}

std::string bson_json(const bson_t* document)
{
    char* json = bson_as_relaxed_extended_json(document, nullptr);
    check_not_null(json);
    if (json == nullptr)
        return {};
    std::string text(json);
    bson_free(json);
    return text;
}

void check_filter_json(const condition_node& root,
                       const mongo_settings& settings_value,
                       const char* expected)
{
    bson_t document = BSON_INITIALIZER;
    mongo_append_filter(&document, root, settings_value);
    
    // Check if the document is empty (no where clause)
    std::string json;
    if (bson_count_keys(&document) == 0) {
        json = "";
    } else {
        json = bson_json(&document);
    }
    
    check_equal(json.c_str(), expected);
    bson_destroy(&document);
}

} // namespace

suite("orm mongo query plan") {
    it("maps equality and range predicates to a BSON filter") {
        plan_fixture fixture;
        fixture.add_predicate("status", ORM_COMPARE_EQUAL, text_value("active"));
        fixture.add_predicate("age", ORM_COMPARE_GREATER_EQUAL, integer_value(18));
        check_filter_json(fixture.where_root, settings(),
                          "{ \"$and\" : [ { \"status\" : \"active\" },"
                          " { \"age\" : { \"$gte\" : 18 } } ] }");
    }

    it("stores the configured id column under _id") {
        plan_fixture fixture;
        fixture.add_predicate("id", ORM_COMPARE_EQUAL, integer_value(5));
        check_filter_json(fixture.where_root, settings(),
                          "{ \"$and\" : [ { \"_id\" : 5 } ] }");
    }

    it("maps NOT EQUAL and range bounds to operator documents") {
        plan_fixture fixture;
        fixture.add_predicate("age", ORM_COMPARE_NOT_EQUAL, integer_value(18));
        fixture.add_predicate("score", ORM_COMPARE_LESS, double_value(0.5));
        fixture.add_predicate("count", ORM_COMPARE_GREATER, integer_value(3));
        check_filter_json(
            fixture.where_root, settings(),
            "{ \"$and\" : [ { \"age\" : { \"$ne\" : 18 } },"
            " { \"score\" : { \"$lt\" : 0.5 } },"
            " { \"count\" : { \"$gt\" : 3 } } ] }");
    }

    it("maps SQL LIKE to an anchored PCRE pattern") {
        plan_fixture fixture;
        fixture.add_predicate("name", ORM_COMPARE_LIKE, text_value("A%"));
        check_filter_json(fixture.where_root, settings(),
                          "{ \"$and\" : [ { \"name\" : { \"$regex\" :"
                          " \"\\\\A(?:A.*)\\\\z\" } } ] }");

        const std::string pattern1 = mongo_like_pattern("a_b%");
        check_equal(pattern1.c_str(),
                     "\\A(?:a.b.*)\\z");
        const std::string pattern2 = mongo_like_pattern("a.b*c\\d");
        check_equal(pattern2.c_str(),
                     "\\A(?:a\\.b\\*c\\\\d)\\z");
    }

    it("maps NOT LIKE to a negated regex") {
        plan_fixture fixture;
        fixture.add_predicate("name", ORM_COMPARE_NOT_LIKE, text_value("A%"));
        check_filter_json(
            fixture.where_root, settings(),
            "{ \"$and\" : [ { \"name\" : { \"$not\" : { \"$regex\" :"
            " \"\\\\A(?:A.*)\\\\z\" } } } ] }");
    }

    it("maps NULL equality to a null filter and NULL inequality to $ne null") {
        plan_fixture null_equal;
        null_equal.add_null_predicate("deleted", ORM_COMPARE_EQUAL);
        check_filter_json(null_equal.where_root, settings(),
                          "{ \"$and\" : [ { \"deleted\" : null } ] }");

        plan_fixture null_not_equal;
        null_not_equal.add_null_predicate("deleted", ORM_COMPARE_NOT_EQUAL);
        check_filter_json(null_not_equal.where_root, settings(),
                          "{ \"$and\" : [ { \"deleted\" : { \"$ne\" : null } } ] }");
    }

    it("maps nested groups to $and and $or arrays") {
        plan_fixture fixture;
        auto inner = std::make_unique<condition_node>();
        inner->is_group = true;
        inner->logic = ORM_LOGIC_OR;
        {
            auto first = std::make_unique<condition_node>();
            first->is_group = false;
            first->value.column = "status";
            first->value.comparison = ORM_COMPARE_EQUAL;
            first->value.parameter = text_value("active");
            inner->children.push_back(std::move(first));
            auto second = std::make_unique<condition_node>();
            second->is_group = false;
            second->value.column = "status";
            second->value.comparison = ORM_COMPARE_EQUAL;
            second->value.parameter = text_value("pending");
            inner->children.push_back(std::move(second));
        }
        fixture.where_root.children.push_back(std::move(inner));
        fixture.add_predicate("country", ORM_COMPARE_EQUAL, text_value("US"));
        check_filter_json(
            fixture.where_root, settings(),
            "{ \"$and\" : [ { \"$or\" : [ { \"status\" : \"active\" },"
            " { \"status\" : \"pending\" } ] },"
            " { \"country\" : \"US\" } ] }");
    }

    it("emits an empty filter when there is no where clause") {
        plan_fixture fixture;
        check_filter_json(fixture.where_root, settings(), "");
    }

    it("builds projection sort and pagination find options") {
        plan_fixture fixture;
        fixture.columns = {"id", "name"};
        fixture.ordering = ordering_spec{false, "score", {}, ORM_ORDER_DESCENDING};
        fixture.offset = 50;
        fixture.limit = 25;

        bson_t options = BSON_INITIALIZER;
        mongo_append_find_options(&options, fixture.view(), settings());
        const std::string json = bson_json(&options);
        check_equal(json.c_str(),
                     "{ \"projection\" : { \"_id\" : 1, \"name\" : 1 },"
                     " \"sort\" : { \"score\" : -1 },"
                     " \"skip\" : 50, \"limit\" : 25 }");
        bson_destroy(&options);
    }

    it("suppresses _id when the id column is not projected") {
        plan_fixture fixture;
        fixture.columns = {"name"};

        bson_t options = BSON_INITIALIZER;
        mongo_append_find_options(&options, fixture.view(), settings());
        const std::string json = bson_json(&options);
        check_equal(json.c_str(),
                     "{ \"projection\" : { \"name\" : 1, \"_id\" : 0 } }");
        bson_destroy(&options);
    }

    it("maps aggregates to a grouped pipeline with having sort and paging") {
        plan_fixture fixture;
        fixture.columns = {"country"};
        fixture.group_columns = {"country"};
        fixture.aggregates = {
            {ORM_AGGREGATE_COUNT_ALL, {}, "total"},
            {ORM_AGGREGATE_SUM, "score", "score_sum"}};
        fixture.add_predicate("active", ORM_COMPARE_EQUAL, integer_value(1));
        auto having = std::make_unique<condition_node>();
        having->is_group = false;
        having->value.column = "total";
        having->value.comparison = ORM_COMPARE_GREATER;
        having->value.parameter = integer_value(1);
        fixture.having_root.children.push_back(std::move(having));
        fixture.ordering = ordering_spec{false, "country", {}, ORM_ORDER_ASCENDING};
        fixture.offset = 5;
        fixture.limit = 10;

        bson_t pipeline = BSON_INITIALIZER;
        mongo_append_pipeline(&pipeline, fixture.view(), settings());
        const std::string json = bson_json(&pipeline);
        check_equal(
            json.c_str(),
            "{ \"0\" : { \"$match\" : { \"$and\" :"
            " [ { \"active\" : 1 } ] } },"
            " \"1\" : { \"$group\" : { \"_id\" : { \"k0\" : \"$country\" },"
            " \"a0\" : { \"$sum\" : 1 },"
            " \"a1\" : { \"$sum\" : \"$score\" } } },"
            " \"2\" : { \"$match\" : { \"a0\" : { \"$gt\" : 1 } } },"
            " \"3\" : { \"$sort\" : { \"_id.k0\" : 1 } },"
            " \"4\" : { \"$skip\" : 5 },"
            " \"5\" : { \"$limit\" : 10 } }");
        bson_destroy(&pipeline);
    }

    it("counts non-null rows for COUNT(column)") {
        plan_fixture fixture;
        fixture.aggregates = {{ORM_AGGREGATE_COUNT, "score", "count_score"}};

        bson_t pipeline = BSON_INITIALIZER;
        mongo_append_pipeline(&pipeline, fixture.view(), settings());
        const std::string json = bson_json(&pipeline);
        check_equal(
            json.c_str(),
            "{ \"0\" : { \"$group\" : { \"_id\" : null, \"a0\" : { \"$sum\" :"
            " { \"$cond\" : { \"if\" : { \"$ne\" : [ \"$score\", null ] },"
            " \"then\" : 1, \"else\" : 0 } } } } } }");
        bson_destroy(&pipeline);
    }

    it("stores the id column as _id on insert and rejects a null id") {
        plan_fixture fixture;
        fixture.kind = query_kind::insert;
        fixture.assignments = {
            {"id", integer_value(1), true},
            {"name", text_value("Alice"), true},
            {"deleted", bound_parameter{}, false}};

        bson_t document = BSON_INITIALIZER;
        mongo_append_insert_document(&document, fixture.view(), settings());
        const std::string json = bson_json(&document);
        check_equal(json.c_str(),
                     "{ \"_id\" : 1, \"name\" : \"Alice\","
                     " \"deleted\" : null }");
        bson_destroy(&document);

        plan_fixture missing_id;
        missing_id.kind = query_kind::insert;
        missing_id.assignments = {{"name", text_value("Bob"), true}};
        bool rejected = false;
        try {
            bson_t missing = BSON_INITIALIZER;
            mongo_append_insert_document(&missing, missing_id.view(), settings());
            bson_destroy(&missing);
        } catch (const status_error& error) {
            rejected = error.status() == ORM_STATUS_INVALID_ARGUMENT;
        }
        check(rejected);
    }

    it("builds a $set update document and rejects id reassignment") {
        plan_fixture fixture;
        fixture.kind = query_kind::update;
        fixture.assignments = {
            {"score", double_value(25.0), true},
            {"deleted", bound_parameter{}, false}};

        bson_t update = BSON_INITIALIZER;
        mongo_append_update_document(&update, fixture.view(), settings());
        const std::string json = bson_json(&update);
        check_equal(json.c_str(),
                     "{ \"$set\" : { \"score\" : 25.0,"
                     " \"deleted\" : null } }");
        bson_destroy(&update);

        plan_fixture reassigning;
        reassigning.kind = query_kind::update;
        reassigning.assignments = {{"id", integer_value(9), true}};
        bool rejected = false;
        try {
            bson_t rejected_update = BSON_INITIALIZER;
            mongo_append_update_document(&rejected_update,
                                         reassigning.view(), settings());
            bson_destroy(&rejected_update);
        } catch (const status_error& error) {
            rejected = error.status() == ORM_STATUS_UNSUPPORTED;
        }
        check(rejected);
    }

    it("maps the id predicate to an _id filter for mutations") {
        const bound_parameter id = integer_value(7);
        bson_t filter = BSON_INITIALIZER;
        mongo_append_id_filter(&filter, id, settings());
        const std::string json = bson_json(&filter);
        check_equal(json.c_str(), "{ \"_id\" : 7 }");
        bson_destroy(&filter);
    }

    it("renders aggregate expression names for having matching") {
        const aggregate_expression count_all{ORM_AGGREGATE_COUNT_ALL, {}, "total"};
        const aggregate_expression sum{ORM_AGGREGATE_SUM, "score", "score_sum"};
        const std::string name1 = mongo_aggregate_sql_name(count_all);
        const std::string name2 = mongo_aggregate_sql_name(sum);
        check_equal(name1.c_str(), "count(*)");
        check_equal(name2.c_str(), "sum(score)");
    }

    it("rejects unsigned ids that exceed the signed BSON range") {
        plan_fixture fixture;
        fixture.kind = query_kind::insert;
        bound_parameter large;
        large.kind = ORM_VALUE_UINT64;
        large.uint64_value = 1ULL << 63;
        fixture.assignments = {{"id", large, true}};
        bool rejected = false;
        try {
            bson_t document = BSON_INITIALIZER;
            mongo_append_insert_document(&document, fixture.view(), settings());
            bson_destroy(&document);
        } catch (const status_error& error) {
            rejected = error.status() == ORM_STATUS_OUT_OF_RANGE;
        }
        check(rejected);
    }

}
