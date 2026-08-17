#ifndef ORM_TIDESDB_ROW_HPP
#define ORM_TIDESDB_ROW_HPP

#include "orm_c_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace orm_c_detail {

struct tidesdb_cell {
    orm_value_kind_t kind = ORM_VALUE_NULL;
    bool is_null = true;
    std::string text;
};

struct tidesdb_field {
    std::string name;
    tidesdb_cell value;
};

using tidesdb_row = std::vector<tidesdb_field>;

std::string encode_tidesdb_row(const tidesdb_row& row,
                               std::size_t maximum_bytes,
                               std::size_t maximum_fields);

tidesdb_row decode_tidesdb_row(const std::uint8_t* data,
                               std::size_t size,
                               std::size_t maximum_bytes,
                               std::size_t maximum_fields);

tidesdb_cell tidesdb_cell_from_parameter(const bound_parameter& parameter);
tidesdb_cell tidesdb_null_cell();

const tidesdb_cell* tidesdb_find_cell(const tidesdb_row& row,
                                      std::string_view column) noexcept;
void tidesdb_set_cell(tidesdb_row& row,
                      std::string column,
                      tidesdb_cell value,
                      std::size_t maximum_fields);

std::string tidesdb_unqualified_column(std::string_view table,
                                       std::string_view column);

bool tidesdb_matches(const condition_node& root,
                     const tidesdb_row& row,
                     std::string_view table);

int tidesdb_compare_cells(const tidesdb_cell& left,
                          const tidesdb_cell& right);

std::string tidesdb_group_key(const std::vector<tidesdb_cell>& values);

} // namespace orm_c_detail

#endif
