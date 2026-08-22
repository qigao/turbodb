#ifndef ORM_TEST_MOCK_ORM_BACKEND_HPP
#define ORM_TEST_MOCK_ORM_BACKEND_HPP

#include "orm_c_internal.hpp"
#include "tinymock.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orm_mock {

struct mock_cell {
  bool is_null = false;
  std::string value;
};

class mock_result_backend final : public orm_c_detail::result_backend {
public:
  mock_result_backend(std::uint64_t rows,
                      std::uint64_t columns,
                      std::uint64_t affected_rows = 0,
                      std::vector<mock_cell> cells = {})
      : rows_(rows), columns_(columns), affected_rows_(affected_rows),
        cells_(std::move(cells)) {}

  std::uint64_t rows() const noexcept override { return rows_; }
  std::uint64_t columns() const noexcept override { return columns_; }
  std::uint64_t affected_rows() const noexcept override { return affected_rows_; }

  bool is_null(std::uint64_t row, std::uint64_t column) const override {
    return get(row, column).is_null;
  }

  vstr cell(std::uint64_t row, std::uint64_t column) const override {
    const mock_cell& selected = get(row, column);
    orm_c_detail::require(!selected.is_null, ORM_STATUS_NULL_VALUE,
                          "mock result cell is SQL NULL");
    return vstr_from_buf(selected.value.data(), selected.value.size());
  }

private:
  const mock_cell& get(std::uint64_t row, std::uint64_t column) const {
    orm_c_detail::require(row < rows_ && column < columns_,
                          ORM_STATUS_OUT_OF_RANGE,
                          "mock result row or column is out of range");
    const std::uint64_t index = row * columns_ + column;
    orm_c_detail::require(index < cells_.size(), ORM_STATUS_INTERNAL_ERROR,
                          "mock result cells are inconsistent");
    return cells_[static_cast<std::size_t>(index)];
  }

  std::uint64_t rows_;
  std::uint64_t columns_;
  std::uint64_t affected_rows_;
  std::vector<mock_cell> cells_;
};

class mock_transaction_backend final : public orm_c_detail::transaction_backend {
public:
  using execute_sql_signature = std::unique_ptr<orm_c_detail::result_backend>(
      std::string_view, const std::vector<orm_c_detail::bound_parameter>&, bool,
      const orm_c_detail::connection_limits&);
  using execute_plan_signature = std::unique_ptr<orm_c_detail::result_backend>(
      const orm_c_detail::query_plan&, const orm_c_detail::connection_limits&);

  std::unique_ptr<orm_c_detail::result_backend>
  execute_sql(std::string_view sql,
              const std::vector<orm_c_detail::bound_parameter>& parameters,
              bool structured_dml,
              const orm_c_detail::connection_limits& limits) override {
    return execute_sql_mock.invoke(sql, parameters, structured_dml, limits);
  }

  std::unique_ptr<orm_c_detail::result_backend>
  execute_plan(const orm_c_detail::query_plan& plan,
               const orm_c_detail::connection_limits& limits) override {
    return execute_plan_mock.invoke(plan, limits);
  }

  void commit() override {
    active_ = false;
    committed_ = true;
  }

  void rollback() override {
    active_ = false;
    rolled_back_ = true;
  }

  void savepoint(std::string_view name) override {
    require_savepoints();
    savepoints_.emplace_back(name);
  }

  void rollback_to_savepoint(std::string_view name) override {
    require_savepoints();
    orm_c_detail::require(has_savepoint(name), ORM_STATUS_INVALID_STATE,
                          "unknown mock savepoint");
  }

  void release_savepoint(std::string_view name) override {
    require_savepoints();
    orm_c_detail::require(has_savepoint(name), ORM_STATUS_INVALID_STATE,
                          "unknown mock savepoint");
  }

  bool active() const noexcept { return active_; }
  bool committed() const noexcept { return committed_; }
  bool rolled_back() const noexcept { return rolled_back_; }
  void enable_savepoints(bool enabled = true) noexcept {
    savepoints_enabled_ = enabled;
  }

  tinymock::function_mock<execute_sql_signature> execute_sql_mock;
  tinymock::function_mock<execute_plan_signature> execute_plan_mock;

private:
  bool has_savepoint(std::string_view name) const {
    for (const std::string& item : savepoints_)
      if (item == name)
        return true;
    return false;
  }

  void require_savepoints() const {
    orm_c_detail::require(savepoints_enabled_, ORM_STATUS_UNSUPPORTED,
                          "mock transaction savepoints are disabled");
  }

  bool active_ = true;
  bool committed_ = false;
  bool rolled_back_ = false;
  bool savepoints_enabled_ = true;
  std::vector<std::string> savepoints_;
};

class mock_database_backend final : public orm_c_detail::database_backend {
public:
  using execute_sql_signature = std::unique_ptr<orm_c_detail::result_backend>(
      std::string_view, const std::vector<orm_c_detail::bound_parameter>&, bool,
      const orm_c_detail::connection_limits&);
  using execute_plan_signature = std::unique_ptr<orm_c_detail::result_backend>(
      const orm_c_detail::query_plan&, const orm_c_detail::connection_limits&);
  using begin_transaction_signature =
      std::unique_ptr<orm_c_detail::transaction_backend>(orm_isolation_t);

  execution_model model() const noexcept override { return execution_model::sql; }

  std::string placeholder(std::size_t one_based_index) const override {
    return placeholder_fn ? placeholder_fn(one_based_index)
                          : "$" + std::to_string(one_based_index);
  }

  std::string pagination(std::optional<std::uint64_t> limit,
                         std::optional<std::uint64_t> offset) const override {
    return pagination_fn ? pagination_fn(limit, offset) : std::string{};
  }

  std::unique_ptr<orm_c_detail::result_backend>
  execute_sql(std::string_view sql,
              const std::vector<orm_c_detail::bound_parameter>& parameters,
              bool structured_dml,
              const orm_c_detail::connection_limits& limits) override {
    return execute_sql_mock.invoke(sql, parameters, structured_dml, limits);
  }

  std::unique_ptr<orm_c_detail::result_backend>
  execute_plan(const orm_c_detail::query_plan& plan,
               const orm_c_detail::connection_limits& limits) override {
    return execute_plan_mock.invoke(plan, limits);
  }

  std::unique_ptr<orm_c_detail::transaction_backend>
  begin_transaction(orm_isolation_t isolation) override {
    return begin_transaction_mock.invoke(isolation);
  }

  std::function<std::string(std::size_t)> placeholder_fn;
  std::function<std::string(std::optional<std::uint64_t>,
                            std::optional<std::uint64_t>)>
      pagination_fn;

  tinymock::function_mock<execute_sql_signature> execute_sql_mock;
  tinymock::function_mock<execute_plan_signature> execute_plan_mock;
  tinymock::function_mock<begin_transaction_signature> begin_transaction_mock;
};

} // namespace orm_mock

#endif
