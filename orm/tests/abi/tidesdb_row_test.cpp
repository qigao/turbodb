#include "row.hpp"

#include <tinytest.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace {

using orm_c_detail::bound_parameter;
using orm_c_detail::condition_node;
using orm_c_detail::tidesdb_cell;
using orm_c_detail::tidesdb_find_cell;
using orm_c_detail::tidesdb_matches;
using orm_c_detail::tidesdb_row;
using orm_c_detail::tidesdb_set_cell;

constexpr std::size_t test_maximum_bytes = 4096;
constexpr std::size_t test_maximum_fields = 16;

bound_parameter text_parameter(std::string value)
{
    bound_parameter parameter;
    parameter.kind = ORM_VALUE_TEXT;
    parameter.text = std::move(value);
    return parameter;
}

bound_parameter double_parameter(double value)
{
    bound_parameter parameter;
    parameter.kind = ORM_VALUE_DOUBLE;
    parameter.double_value = value;
    parameter.text = std::to_string(value);
    return parameter;
}

condition_node predicate_node(std::string column,
                              orm_compare_t comparison,
                              bound_parameter parameter)
{
    condition_node node;
    node.is_group = false;
    node.value.column = std::move(column);
    node.value.comparison = comparison;
    node.value.parameter = std::move(parameter);
    return node;
}

} // namespace

spec("TidesDB ORM row codec") { 
    given("a row containing every ORM value category") {
        when("the versioned binary representation is round-tripped") {
            then("field names, kinds, nulls, and embedded bytes are preserved") {
                tidesdb_row source;
                tidesdb_set_cell(source, "id", {ORM_VALUE_INT64, false, "-7"},
                                 test_maximum_fields);
                tidesdb_set_cell(source, "generation", {ORM_VALUE_UINT64, false, "9"},
                                 test_maximum_fields);
                tidesdb_set_cell(source, "score", {ORM_VALUE_DOUBLE, false, "12.5"},
                                 test_maximum_fields);
                tidesdb_set_cell(source, "active", {ORM_VALUE_BOOLEAN, false, "1"},
                                 test_maximum_fields);
                tidesdb_set_cell(source, "name",
                                 {ORM_VALUE_TEXT, false, std::string("A:B", 3)},
                                 test_maximum_fields);
                tidesdb_set_cell(source, "note", orm_c_detail::tidesdb_null_cell(),
                                 test_maximum_fields);
                tidesdb_set_cell(
                    source, "payload",
                    {ORM_VALUE_BLOB, false, std::string("A\0B\0C", 5)},
                    test_maximum_fields);

                const std::string encoded = orm_c_detail::encode_tidesdb_row(
                    source, test_maximum_bytes, test_maximum_fields);
                const tidesdb_row decoded = orm_c_detail::decode_tidesdb_row(
                    reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size(),
                    test_maximum_bytes, test_maximum_fields);

                check_equal(decoded.size(), source.size());
                const tidesdb_cell* name = tidesdb_find_cell(decoded, "name");
                check_not_null(name);
                if (name != nullptr) {
                    check_equal(name->kind, ORM_VALUE_TEXT);
                    check_equal(name->text.size(), 3);
                    check(name->text == std::string("A:B", 3));
                }
                const tidesdb_cell* note = tidesdb_find_cell(decoded, "note");
                check_not_null(note);
                if (note != nullptr)
                    check(note->is_null);
                const tidesdb_cell* payload = tidesdb_find_cell(decoded, "payload");
                check_not_null(payload);
                if (payload != nullptr) {
                    check_equal(payload->kind, ORM_VALUE_BLOB);
                    check(payload->text == std::string("A\0B\0C", 5));
                }
            }
        }
    }

    given("a materialized row and qualified nested predicates") {
        when("WHERE and aggregate HAVING conditions are evaluated") {
            then("SQL null logic and exact aggregate expression names are respected") {
                tidesdb_row row;
                tidesdb_set_cell(row, "status", {ORM_VALUE_TEXT, false, "active"},
                                 test_maximum_fields);
                tidesdb_set_cell(row, "sum(score)", {ORM_VALUE_DOUBLE, false, "40"},
                                 test_maximum_fields);

                condition_node where;
                where.children.push_back(std::make_unique<condition_node>(
                    predicate_node("person.status", ORM_COMPARE_EQUAL,
                                   text_parameter("active"))));
                check(tidesdb_matches(where, row, "person"));

                condition_node having;
                having.children.push_back(std::make_unique<condition_node>(
                    predicate_node("sum(score)", ORM_COMPARE_GREATER,
                                   double_parameter(30.0))));
                check(tidesdb_matches(having, row, "person"));
            }
        }
    }

    given("truncated or version-incompatible persisted bytes") {
        when("the decoder validates the row envelope") {
            then("it rejects corruption instead of returning a partial row") {
                tidesdb_row source;
                tidesdb_set_cell(source, "id", {ORM_VALUE_UINT64, false, "1"},
                                 test_maximum_fields);
                std::string encoded = orm_c_detail::encode_tidesdb_row(
                    source, test_maximum_bytes, test_maximum_fields);

                bool rejected_version = false;
                encoded[6] = static_cast<char>(2);
                try {
                    (void)orm_c_detail::decode_tidesdb_row(
                        reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size(),
                        test_maximum_bytes, test_maximum_fields);
                } catch (const orm_c_detail::status_error& error) {
                    rejected_version = error.status() == ORM_STATUS_DATASTORE_ERROR;
                }
                check(rejected_version);

                encoded[6] = static_cast<char>(1);
                encoded.pop_back();
                bool rejected_truncation = false;
                try {
                    (void)orm_c_detail::decode_tidesdb_row(
                        reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size(),
                        test_maximum_bytes, test_maximum_fields);
                } catch (const orm_c_detail::status_error& error) {
                    rejected_truncation = error.status() == ORM_STATUS_DATASTORE_ERROR;
                }
                check(rejected_truncation);
            }
        }
    }
}
