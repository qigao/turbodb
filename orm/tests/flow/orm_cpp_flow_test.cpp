#include <orm.hpp>

#include <cmeta/struct.h>
#include <tinytest.hpp>

#include <cstddef>
#include <utility>

#define ORM_CPP_FLOW_DATA_PREFIX_SIZE                                        \
  (offsetof(cmeta_data_desc, shape) +                                        \
   sizeof(((cmeta_data_desc *)0)->shape))

Struct(orm_cpp_flow_row,
    (int, id),
    (long, score)
);

static const cmeta_type_identity orm_cpp_flow_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.CppFlowRow");
static const cmeta_type_traits orm_cpp_flow_row_traits = {
    CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static const cmeta_type_desc orm_cpp_flow_row_type = {
    "orm_cpp_flow_row", sizeof(orm_cpp_flow_row), alignof(orm_cpp_flow_row),
    CMETA_T_OBJECT, nullptr, &orm_cpp_flow_row_traits,
    &orm_cpp_flow_row_identity};
static const cmeta_data_field_desc orm_cpp_flow_row_fields[] = {
    {"orm.test.CppFlowRow.id", "id", offsetof(orm_cpp_flow_row, id),
     &cmeta_data_int},
    {"orm.test.CppFlowRow.score", "score", offsetof(orm_cpp_flow_row, score),
     &cmeta_data_long}};
static const cmeta_data_struct_shape orm_cpp_flow_row_shape = {
    StructMeta(orm_cpp_flow_row), orm_cpp_flow_row_fields, 2u};
static const cmeta_data_desc orm_cpp_flow_row_data = {
    ORM_CPP_FLOW_DATA_PREFIX_SIZE, CMETA_DATA_DESC_ABI_VERSION,
    "orm.test.CppFlowRow.data", "CppFlowRow", CMETA_DATA_STRUCT,
    &orm_cpp_flow_row_type, &orm_cpp_flow_row_shape,
    nullptr, nullptr, nullptr};

spec("ORM thin C++ CFlow facade") {
  it("only owns C handles and forwards typed Source demand") {
    orm::connection connection(orm::config("sqlite").option("filename", ":memory:"));
    auto rows = std::move(connection.raw(
        "select 7 as id, 19 as score union all select 11, 29 order by id"))
                    .open<orm_cpp_flow_row>(orm_cpp_flow_row_data);
    orm_cpp_flow_row first{};
    orm_cpp_flow_row second{};
    check_equal(rows.next(first).kind, CFLOW_STEP_VALUE);
    check_equal(first.id, 7);
    check_equal(first.score, 19L);
    check_equal(rows.next(second).kind, CFLOW_STEP_VALUE);
    check_equal(second.id, 11);
    check_equal(second.score, 29L);
    check_equal(rows.next(second).kind, CFLOW_STEP_DONE);
  }

  it("forwards command demand and affected rows through the C Source") {
    orm::connection connection(orm::config("sqlite").option("filename", ":memory:"));
    auto create = std::move(connection.raw(
        "create table cpp_flow_command(id integer, score integer)"))
                      .execute();
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(create.next(result).kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(result.affected_rows, uint64_t{0});

    auto insert = std::move(connection.raw(
        "insert into cpp_flow_command values(7, 19)"))
                      .execute();
    result = ORM_COMMAND_RESULT_INIT;
    check_equal(insert.next(result).kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(result.affected_rows, uint64_t{1});

    auto rows = std::move(connection.raw(
        "select id, score from cpp_flow_command"))
                    .open<orm_cpp_flow_row>(orm_cpp_flow_row_data);
    orm_cpp_flow_row row{};
    check_equal(rows.next(row).kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
    check_equal(row.score, 19L);
  }
}
