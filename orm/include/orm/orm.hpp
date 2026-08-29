#ifndef ORM_HPP
#define ORM_HPP

#include "orm.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace orm {

class status_error final : public std::runtime_error {
public:
  status_error(orm_status_t status, const char *message)
      : std::runtime_error(message ? message : orm_status_message(status)),
        status_(status) {}
  [[nodiscard]] orm_status_t status() const noexcept { return status_; }
private:
  orm_status_t status_;
};

namespace detail {
inline orm_string_view_t view(std::string_view value) noexcept {
  return {value.data(), value.size()};
}
inline void check(orm_status_t status, const orm_error_t &error) {
  if (status != ORM_STATUS_OK) throw status_error(status, error.message);
}
inline orm_value_t value(std::nullptr_t) noexcept { return orm_null(); }
inline orm_value_t value(bool input) noexcept { return orm_bool(input); }
inline orm_value_t value(float input) noexcept { return orm_f64(input); }
inline orm_value_t value(double input) noexcept { return orm_f64(input); }
inline orm_value_t value(const char *input) noexcept { return orm_text(input); }
inline orm_value_t value(std::string_view input) noexcept {
  return orm_text_v(view(input));
}
inline orm_value_t value(const std::string &input) noexcept {
  return value(std::string_view(input));
}
template <typename T, std::enable_if_t<std::is_integral_v<T> &&
                                           std::is_signed_v<T> &&
                                           !std::is_same_v<T, bool>, int> = 0>
inline orm_value_t value(T input) noexcept {
  return orm_i64(static_cast<std::int64_t>(input));
}
template <typename T, std::enable_if_t<std::is_integral_v<T> &&
                                           std::is_unsigned_v<T> &&
                                           !std::is_same_v<T, bool>, int> = 0>
inline orm_value_t value(T input) noexcept {
  return orm_u64(static_cast<std::uint64_t>(input));
}
} // namespace detail

class config final {
public:
  explicit config(std::string driver) : driver_(std::move(driver)) {
    orm_config(&limits_);
  }
  config &option(std::string keyword, std::string value) {
    options_.emplace_back(std::move(keyword), std::move(value));
    return *this;
  }
  config &max_result_rows(std::uint64_t value) noexcept {
    limits_.max_result_rows = value;
    return *this;
  }
  config &max_result_bytes(std::uint64_t value) noexcept {
    limits_.max_result_bytes = value;
    return *this;
  }
private:
  friend class connection;
  std::string driver_;
  std::vector<std::pair<std::string, std::string>> options_;
  orm_config_t limits_{};
};

class transaction;

template <typename Row> class source final {
public:
  source(const source &) = delete;
  source &operator=(const source &) = delete;
  source(source &&other) noexcept
      : query_(std::exchange(other.query_, nullptr)),
        source_(std::exchange(other.source_, cflow_source{})) {}
  source &operator=(source &&other) noexcept {
    if (this != &other) {
      reset();
      query_ = std::exchange(other.query_, nullptr);
      source_ = std::exchange(other.source_, cflow_source{});
    }
    return *this;
  }
  ~source() { reset(); }
  [[nodiscard]] cflow_step next(Row &row) {
    return cflow_source_resume(&source_, nullptr, &row);
  }
  [[nodiscard]] cflow_step next(Row &row, cflow_resume_ctx &context) {
    return cflow_source_resume(&source_, &context, &row);
  }
  void cancel() noexcept {
    if (cflow_source_valid(&source_)) cflow_source_cancel(&source_);
  }
  [[nodiscard]] cflow_source *native_handle() noexcept { return &source_; }
private:
  friend class query;
  source(orm_query_t *query, cflow_source native) noexcept
      : query_(query), source_(native) {}
  void reset() noexcept {
    if (cflow_source_valid(&source_)) cflow_source_destroy(&source_);
    source_ = {};
    orm_query_destroy(query_);
    query_ = nullptr;
  }
  orm_query_t *query_ = nullptr;
  cflow_source source_{};
};

class query final {
public:
  query(const query &) = delete;
  query &operator=(const query &) = delete;
  query(query &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  query &operator=(query &&other) noexcept {
    if (this != &other) {
      orm_query_destroy(handle_);
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  ~query() { orm_query_destroy(handle_); }

  query &column(std::string_view name) {
    call([&](orm_error_t *e) {
      return orm_query_add_column(handle_, detail::view(name), e);
    });
    return *this;
  }
  query &all() {
    call([&](orm_error_t *e) { return orm_query_select_all(handle_, e); });
    return *this;
  }
  template <typename T> query &set(std::string_view name, T &&input) {
    const orm_value_t encoded = detail::value(std::forward<T>(input));
    call([&](orm_error_t *e) {
      return orm_query_set(handle_, detail::view(name), encoded, e);
    });
    return *this;
  }
  template <typename T>
  query &where(std::string_view name, orm_compare_t comparison, T &&input) {
    const orm_value_t encoded = detail::value(std::forward<T>(input));
    call([&](orm_error_t *e) {
      return orm_query_where(handle_, detail::view(name), comparison, encoded, e);
    });
    return *this;
  }
  query &where_key(const orm_key_part_t *parts, std::uint32_t count) {
    call([&](orm_error_t *e) {
      return orm_query_where_key(handle_, parts, count, e);
    });
    return *this;
  }
  template <typename T> query &bind(T &&input) {
    const orm_value_t encoded = detail::value(std::forward<T>(input));
    call([&](orm_error_t *e) { return orm_query_bind(handle_, encoded, e); });
    return *this;
  }
  query &limit(std::uint64_t value) {
    call([&](orm_error_t *e) { return orm_query_set_limit(handle_, value, e); });
    return *this;
  }
  query &offset(std::uint64_t value) {
    call([&](orm_error_t *e) { return orm_query_set_offset(handle_, value, e); });
    return *this;
  }
  query &order_by(std::string_view name, orm_order_t order) {
    call([&](orm_error_t *e) {
      return orm_query_order_by(handle_, detail::view(name), order, e);
    });
    return *this;
  }
  template <typename Row>
  [[nodiscard]] source<Row> open(const cmeta_data_desc &row_shape) && {
    orm_flow_config_t config;
    orm_flow_config(&config, &row_shape);
    cflow_source native{};
    orm_error_t error;
    orm_error_init(&error);
    detail::check(orm_query_open_flow(handle_, &config, &native, &error), error);
    return source<Row>(std::exchange(handle_, nullptr), native);
  }
  [[nodiscard]] source<orm_command_result_t> execute() && {
    cflow_source native{};
    orm_error_t error;
    orm_error_init(&error);
    detail::check(orm_query_open_command_flow(handle_, &native, &error), error);
    return source<orm_command_result_t>(std::exchange(handle_, nullptr), native);
  }
  template <typename Row>
  [[nodiscard]] source<Row> open(transaction &owner,
                                 const cmeta_data_desc &row_shape) &&;
  [[nodiscard]] source<orm_command_result_t> execute(transaction &owner) &&;
  [[nodiscard]] orm_query_t *native_handle() noexcept { return handle_; }
private:
  friend class connection;
  explicit query(orm_query_t *handle) noexcept : handle_(handle) {}
  template <typename F> void call(F &&function) {
    orm_error_t error;
    orm_error_init(&error);
    detail::check(function(&error), error);
  }
  orm_query_t *handle_ = nullptr;
};

class connection final {
public:
  using connector = orm_status_t(ORM_C_CALL *)(
      const orm_config_t *, orm_connection_t **, orm_error_t *);

  explicit connection(const config &configuration)
      : connection(configuration, orm_connect) {}
  connection(const config &configuration, connector connect) {
    std::vector<orm_option_t> options;
    options.reserve(configuration.options_.size());
    for (const auto &entry : configuration.options_)
      options.push_back({detail::view(entry.first), detail::view(entry.second)});
    orm_config_t native = configuration.limits_;
    native.driver = detail::view(configuration.driver_);
    native.options = options.empty() ? nullptr : options.data();
    native.option_count = static_cast<std::uint32_t>(options.size());
    orm_error_t error;
    orm_error_init(&error);
    if (connect == nullptr)
      throw status_error(ORM_STATUS_INVALID_ARGUMENT,
                         "connection connector is null");
    detail::check(connect(&native, &handle_, &error), error);
  }
  connection(const connection &) = delete;
  connection &operator=(const connection &) = delete;
  connection(connection &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  connection &operator=(connection &&other) noexcept {
    if (this != &other) {
      orm_disconnect(handle_);
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  ~connection() { orm_disconnect(handle_); }
  [[nodiscard]] query select(std::string_view table) {
    return make(orm_query_create, table);
  }
  [[nodiscard]] query insert(std::string_view table) {
    return make(orm_insert, table);
  }
  [[nodiscard]] query update(std::string_view table) {
    return make(orm_update, table);
  }
  [[nodiscard]] query remove(std::string_view table) {
    return make(orm_delete, table);
  }
  [[nodiscard]] query raw(std::string_view sql) { return make(orm_raw, sql); }
  [[nodiscard]] transaction begin(
      orm_isolation_t isolation = ORM_ISOLATION_SERIALIZABLE);
private:
  using factory = orm_status_t(ORM_C_CALL *)(orm_connection_t *,
      orm_string_view_t, orm_query_t **, orm_error_t *);
  query make(factory function, std::string_view input) {
    orm_query_t *output = nullptr;
    orm_error_t error;
    orm_error_init(&error);
    detail::check(function(handle_, detail::view(input), &output, &error), error);
    return query(output);
  }
  orm_connection_t *handle_ = nullptr;
};

class transaction final {
public:
  transaction(const transaction &) = delete;
  transaction &operator=(const transaction &) = delete;
  transaction(transaction &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  transaction &operator=(transaction &&other) noexcept {
    if (this != &other) {
      orm_transaction_destroy(handle_);
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  ~transaction() { orm_transaction_destroy(handle_); }
  void commit() { invoke(orm_transaction_commit); }
  void rollback() { invoke(orm_transaction_rollback); }
  [[nodiscard]] orm_transaction_t *native_handle() noexcept { return handle_; }
private:
  friend class connection;
  friend class query;
  explicit transaction(orm_transaction_t *handle) noexcept : handle_(handle) {}
  using operation = orm_status_t(ORM_C_CALL *)(orm_transaction_t *, orm_error_t *);
  void invoke(operation function) {
    orm_error_t error;
    orm_error_init(&error);
    detail::check(function(handle_, &error), error);
  }
  orm_transaction_t *handle_ = nullptr;
};

inline transaction connection::begin(orm_isolation_t isolation) {
  orm_transaction_t *output = nullptr;
  orm_error_t error;
  orm_error_init(&error);
  detail::check(orm_transaction_begin(handle_, isolation, &output, &error), error);
  return transaction(output);
}

template <typename Row>
source<Row> query::open(transaction &owner,
                        const cmeta_data_desc &row_shape) && {
  orm_flow_config_t config;
  orm_flow_config(&config, &row_shape);
  cflow_source native{};
  orm_error_t error;
  orm_error_init(&error);
  detail::check(orm_query_open_flow_in_transaction(
                    handle_, owner.handle_, &config, &native, &error), error);
  return source<Row>(std::exchange(handle_, nullptr), native);
}

inline source<orm_command_result_t> query::execute(transaction &owner) && {
  cflow_source native{};
  orm_error_t error;
  orm_error_init(&error);
  detail::check(orm_query_open_command_flow_in_transaction(
                    handle_, owner.handle_, &native, &error), error);
  return source<orm_command_result_t>(std::exchange(handle_, nullptr), native);
}

} // namespace orm

#endif
