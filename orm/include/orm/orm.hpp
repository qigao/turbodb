#ifndef ORM_HPP
#define ORM_HPP

/*
 * QueryDSL-inspired ORM API for C++17.
 *
 * This header adds typed schema descriptors, composable predicates, fluent
 * query builders, RAII ownership, and exceptions over the stable C ABI.
 */

#include "orm.h"
#include "model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace orm {

  enum class comparison : orm_compare_t {
    equal = ORM_COMPARE_EQUAL,
    not_equal = ORM_COMPARE_NOT_EQUAL,
    less = ORM_COMPARE_LESS,
    less_equal = ORM_COMPARE_LESS_EQUAL,
    greater = ORM_COMPARE_GREATER,
    greater_equal = ORM_COMPARE_GREATER_EQUAL,
    like = ORM_COMPARE_LIKE,
    not_like = ORM_COMPARE_NOT_LIKE
  };

  enum class subquery_quantifier : orm_subquery_quantifier_t {
    any = ORM_SUBQUERY_ANY,
    all = ORM_SUBQUERY_ALL
  };

  enum class sort_order : orm_order_t {
    ascending = ORM_ORDER_ASCENDING,
    descending = ORM_ORDER_DESCENDING
  };

  enum class logic : orm_logic_t { and_ = ORM_LOGIC_AND, or_ = ORM_LOGIC_OR };

  enum class join_type : orm_join_t { inner = ORM_JOIN_INNER, left = ORM_JOIN_LEFT };

  enum class aggregate_function : orm_aggregate_t {
    count_all = ORM_AGGREGATE_COUNT_ALL,
    count = ORM_AGGREGATE_COUNT,
    sum = ORM_AGGREGATE_SUM,
    average = ORM_AGGREGATE_AVERAGE,
    minimum = ORM_AGGREGATE_MINIMUM,
    maximum = ORM_AGGREGATE_MAXIMUM
  };

  enum class isolation_level : orm_isolation_t {
    read_uncommitted = ORM_ISOLATION_READ_UNCOMMITTED,
    read_committed = ORM_ISOLATION_READ_COMMITTED,
    repeatable_read = ORM_ISOLATION_REPEATABLE_READ,
    snapshot = ORM_ISOLATION_SNAPSHOT,
    serializable = ORM_ISOLATION_SERIALIZABLE
  };

  enum class transaction_propagation : std::uint8_t {
    required,
    requires_new,
    mandatory,
    supports,
    not_supported,
    never
  };

  class status_error final : public std::runtime_error {
  public:
    status_error(orm_status_t status, std::string message)
        : std::runtime_error(std::move(message)), status_(status) {}

    [[nodiscard]] orm_status_t status() const noexcept { return status_; }

  private:
    orm_status_t status_;
  };

  namespace detail {

    struct aggregate_expression_factory;

    inline orm_string_view_t to_view(std::string_view value) noexcept {
      return {value.data(), value.size()};
    }

    inline std::string diagnostic(const orm_error_t &error, orm_status_t status,
                                  std::string_view operation) {
      const char *begin = error.message;
      const char *end = std::find(begin, begin + ORM_C_ERROR_MESSAGE_CAPACITY, '\0');
      std::string message(operation);
      message += ": ";
      if (begin != end) message.append(begin, end);
      else message += orm_status_message(status);
      return message;
    }

    template <typename Operation> void check(std::string_view operation, Operation &&invoke) {
      orm_error_t error;
      orm_error_init(&error);
      const orm_status_t status = invoke(&error);
      if (status != ORM_STATUS_OK) throw status_error(status, diagnostic(error, status, operation));
    }

  } // namespace detail

  class value final {
  public:
    value(std::nullptr_t) noexcept : storage_(std::monostate{}) {}

    value(std::nullopt_t) noexcept : value(nullptr) {}

    template <typename T> value(const std::optional<T> &input) : value(nullptr) {
      if (input.has_value()) *this = value(*input);
    }

    template <typename T> value(std::optional<T> &&input) : value(nullptr) {
      if (input.has_value()) *this = value(std::move(*input));
    }

    value(bool input) noexcept : storage_(input) {}

    template <typename Integer, std::enable_if_t<std::is_integral_v<std::decay_t<Integer>> &&
                                                     std::is_signed_v<std::decay_t<Integer>> &&
                                                     !std::is_same_v<std::decay_t<Integer>, bool>,
                                                 int> = 0>
    value(Integer input) noexcept : storage_(static_cast<std::int64_t>(input))
    {
        static_assert(sizeof(Integer) <= sizeof(std::int64_t),
                      "ORM value cannot represent an integer wider than 64 bits");
    }

    template <typename Integer, std::enable_if_t<std::is_integral_v<std::decay_t<Integer>> &&
                                                     std::is_unsigned_v<std::decay_t<Integer>> &&
                                                     !std::is_same_v<std::decay_t<Integer>, bool>,
                                                 int> = 0>
    value(Integer input) noexcept : storage_(static_cast<std::uint64_t>(input))
    {
        static_assert(sizeof(Integer) <= sizeof(std::uint64_t),
                      "ORM value cannot represent an integer wider than 64 bits");
    }

    template <typename Floating,
              std::enable_if_t<std::is_floating_point_v<std::decay_t<Floating>>, int> = 0>
    value(Floating input) noexcept : storage_(static_cast<double>(input)) {}

    template <typename Enum, std::enable_if_t<std::is_enum_v<std::decay_t<Enum>>, int> = 0>
    value(Enum input) noexcept
        : value(static_cast<std::underlying_type_t<std::decay_t<Enum>>>(input)) {}

    value(const char *input) {
      if (input == nullptr) throw std::invalid_argument("ORM text value is null");
      storage_ = std::string(input);
    }

    value(std::string input) : storage_(std::move(input)) {}

    value(std::string_view input) : storage_(std::string(input)) {}

    value(std::vector<std::uint8_t> input) : storage_(std::move(input)) {}

  private:
    friend class query;

    [[nodiscard]] orm_value_t native() const noexcept {
      orm_value_t result{};
      if (std::holds_alternative<std::monostate>(storage_)) {
        result.kind = ORM_VALUE_NULL;
      } else if (const auto *signed_value = std::get_if<std::int64_t>(&storage_)) {
        result.kind = ORM_VALUE_INT64;
        result.data.int64_value = *signed_value;
      } else if (const auto *unsigned_value = std::get_if<std::uint64_t>(&storage_)) {
        result.kind = ORM_VALUE_UINT64;
        result.data.uint64_value = *unsigned_value;
      } else if (const auto *real_value = std::get_if<double>(&storage_)) {
        result.kind = ORM_VALUE_DOUBLE;
        result.data.double_value = *real_value;
      } else if (const auto *boolean_value = std::get_if<bool>(&storage_)) {
        result.kind = ORM_VALUE_BOOLEAN;
        result.data.boolean_value = static_cast<std::uint8_t>(*boolean_value);
      } else if (const auto *blob_value = std::get_if<std::vector<std::uint8_t>>(&storage_)) {
        result.kind = ORM_VALUE_BLOB;
        result.data.blob_value = orm_blob_t{blob_value->data(), blob_value->size()};
      } else {
        const auto &text_value = std::get<std::string>(storage_);
        result.kind = ORM_VALUE_TEXT;
        result.data.text_value = detail::to_view(text_value);
      }
      return result;
    }

    std::variant<std::monostate, std::int64_t, std::uint64_t, double, bool,
                 std::string, std::vector<std::uint8_t>>
        storage_;
  };

  template <typename Entity> class table;
  template <typename T> class column;
  template <typename T> class aggregate_expression;
  template <typename T> class expression;

  namespace detail {

    struct scalar_token_spec {
      orm_scalar_token_kind_t kind;
      std::string column;
      value operand;
    };
    template <typename T>
    struct scalar_expression_factory {
      static expression<T> from_tokens(std::vector<scalar_token_spec> tokens,
                                       std::string alias = {});
      template <typename Input>
      static expression<T> from_value(Input input);
    };

    template <typename T> struct scalar_type { using type = std::decay_t<T>; };
    template <typename T> struct scalar_type<std::optional<T>> { using type = std::decay_t<T>; };
    template <typename T> using scalar_type_t = typename scalar_type<std::decay_t<T>>::type;

    template <typename T>
    inline constexpr bool is_text_v =
        std::is_same_v<scalar_type_t<T>, std::string> ||
        std::is_same_v<scalar_type_t<T>, std::string_view> ||
        (std::is_pointer_v<scalar_type_t<T>> &&
         std::is_same_v<std::remove_cv_t<std::remove_pointer_t<scalar_type_t<T>>>, char>);

    template <typename T>
    inline constexpr bool is_number_v =
        (std::is_arithmetic_v<scalar_type_t<T>> && !std::is_same_v<scalar_type_t<T>, bool>) ||
        std::is_enum_v<scalar_type_t<T>>;

    template <typename T>
    inline constexpr bool is_blob_v =
        std::is_same_v<scalar_type_t<T>, std::vector<std::uint8_t>>;

    template <typename Field, typename Input>
    inline constexpr bool is_compatible_value_v =
        std::is_same_v<std::decay_t<Input>, std::nullptr_t> ||
        std::is_same_v<std::decay_t<Input>, std::nullopt_t> ||
        (std::is_same_v<scalar_type_t<Field>, bool> &&
         std::is_same_v<scalar_type_t<Input>, bool>) ||
        (is_number_v<Field> && is_number_v<Input>) || (is_text_v<Field> && is_text_v<Input>) ||
        (is_blob_v<Field> && is_blob_v<Input>);

    template <typename Left, typename Right>
    inline constexpr bool is_compatible_column_v =
        (std::is_same_v<scalar_type_t<Left>, bool> &&
         std::is_same_v<scalar_type_t<Right>, bool>) ||
        (is_number_v<Left> && is_number_v<Right>) || (is_text_v<Left> && is_text_v<Right>);

    struct condition_leaf {
      std::string table;
      std::string column;
      comparison operation;
      value operand;
    };

    struct column_condition_leaf {
      std::string left_table;
      std::string left_column;
      comparison operation;
      std::string right_table;
      std::string right_column;
    };

    struct aggregate_leaf {
      aggregate_function function;
      std::string column;
      comparison operation;
      value operand;
    };

    struct predicate_node;

    struct condition_group {
      logic operation;
      std::vector<std::shared_ptr<const predicate_node>> children;
    };

    struct predicate_node {
      using storage =
          std::variant<condition_leaf, column_condition_leaf, aggregate_leaf, condition_group>;

      explicit predicate_node(condition_leaf leaf) : data(std::move(leaf)) {}
      explicit predicate_node(column_condition_leaf leaf) : data(std::move(leaf)) {}
      explicit predicate_node(aggregate_leaf leaf) : data(std::move(leaf)) {}
      explicit predicate_node(condition_group group) : data(std::move(group)) {}

      storage data;
    };

    struct predicate_access;

  } // namespace detail

  class predicate final {
  public:
    predicate(const predicate &) = default;
    predicate(predicate &&) noexcept = default;
    predicate &operator=(const predicate &) = default;
    predicate &operator=(predicate &&) noexcept = default;

  private:
    template <typename> friend class column;
    template <typename> friend class aggregate_expression;
    friend struct detail::predicate_access;
    friend predicate operator&&(predicate left, predicate right);
    friend predicate operator||(predicate left, predicate right);

    explicit predicate(std::shared_ptr<const detail::predicate_node> root)
        : root_(std::move(root)) {}

    static predicate condition(std::string table_name, std::string column_name,
                               comparison operation, value operand) {
      return predicate(std::make_shared<const detail::predicate_node>(detail::condition_leaf{
          std::move(table_name), std::move(column_name), operation, std::move(operand)}));
    }

    static predicate aggregate(aggregate_function function, std::string column_name,
                               comparison operation, value operand) {
      return predicate(std::make_shared<const detail::predicate_node>(
          detail::aggregate_leaf{function, std::move(column_name), operation, std::move(operand)}));
    }

    static predicate columns(std::string left_table, std::string left_column,
                             comparison operation, std::string right_table,
                             std::string right_column) {
      return predicate(std::make_shared<const detail::predicate_node>(
          detail::column_condition_leaf{std::move(left_table), std::move(left_column), operation,
                                        std::move(right_table), std::move(right_column)}));
    }

    static predicate combine(logic operation, predicate left, predicate right) {
      std::vector<std::shared_ptr<const detail::predicate_node>> children;
      auto append = [&](std::shared_ptr<const detail::predicate_node> node) {
        const auto *group = std::get_if<detail::condition_group>(&node->data);
        if (group != nullptr && group->operation == operation) {
          children.insert(children.end(), group->children.begin(), group->children.end());
        } else {
          children.push_back(std::move(node));
        }
      };
      append(std::move(left.root_));
      append(std::move(right.root_));
      return predicate(std::make_shared<const detail::predicate_node>(
          detail::condition_group{operation, std::move(children)}));
    }

    std::shared_ptr<const detail::predicate_node> root_;
  };

  inline predicate operator&&(predicate left, predicate right) {
    return predicate::combine(logic::and_, std::move(left), std::move(right));
  }

  inline predicate operator||(predicate left, predicate right) {
    return predicate::combine(logic::or_, std::move(left), std::move(right));
  }

  class join_condition final {
  public:
    [[nodiscard]] comparison operation() const noexcept { return operation_; }

  private:
    template <typename> friend class column;
    friend class join_builder;

    join_condition(std::string left_table, std::string left_column, comparison operation,
                   std::string right_table, std::string right_column)
        : left_table_(std::move(left_table)), left_column_(std::move(left_column)),
          operation_(operation), right_table_(std::move(right_table)),
          right_column_(std::move(right_column)) {}

    std::string left_table_;
    std::string left_column_;
    comparison operation_;
    std::string right_table_;
    std::string right_column_;
  };

  class order_specifier final {
  public:
    [[nodiscard]] std::string_view column_name() const noexcept { return column_; }
    [[nodiscard]] sort_order order() const noexcept { return order_; }

  private:
    template <typename> friend class column;

    order_specifier(std::string column_name, sort_order order)
        : column_(std::move(column_name)), order_(order) {}

    std::string column_;
    sort_order order_;
  };

  template <typename Entity> class table {
  public:
    using entity_type = Entity;

    explicit table(std::string name) : name_(std::move(name)) {
      if (name_.empty()) throw std::invalid_argument("ORM table name is empty");
    }

    [[nodiscard]] std::string_view name() const noexcept { return name_; }

  private:
    std::string name_;
  };

  template <typename T> class column final {
  public:
    using value_type = T;

    template <typename Entity>
    column(const table<Entity> &owner, std::string name)
        : table_(owner.name()), name_(std::move(name)), qualified_(table_ + "." + name_) {
      if (name_.empty()) throw std::invalid_argument("ORM column name is empty");
    }

    [[nodiscard]] std::string_view table_name() const noexcept { return table_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] std::string_view qualified_name() const noexcept { return qualified_; }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate eq(Input &&input) const {
      return compare(comparison::equal, std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate ne(Input &&input) const {
      return compare(comparison::not_equal, std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate lt(Input &&input) const {
      return compare(comparison::less, std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate le(Input &&input) const {
      return compare(comparison::less_equal, std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate gt(Input &&input) const {
      return compare(comparison::greater, std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate ge(Input &&input) const {
      return compare(comparison::greater_equal, std::forward<Input>(input));
    }

    template <
        typename Input,
        std::enable_if_t<detail::is_text_v<T> && detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate like(Input &&input) const {
      return compare(comparison::like, std::forward<Input>(input));
    }

    template <
        typename Input,
        std::enable_if_t<detail::is_text_v<T> && detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate not_like(Input &&input) const {
      return compare(comparison::not_like, std::forward<Input>(input));
    }

    [[nodiscard]] predicate is_null() const { return eq(nullptr); }

    [[nodiscard]] predicate is_not_null() const { return ne(nullptr); }

    template <typename Lower, typename Upper,
              std::enable_if_t<detail::is_compatible_value_v<T, Lower> &&
                                   detail::is_compatible_value_v<T, Upper>,
                               int> = 0>
    [[nodiscard]] predicate between(Lower &&lower, Upper &&upper) const {
      return ge(std::forward<Lower>(lower)) && le(std::forward<Upper>(upper));
    }

    template <typename Lower, typename Upper,
              std::enable_if_t<detail::is_compatible_value_v<T, Lower> &&
                                   detail::is_compatible_value_v<T, Upper>,
                               int> = 0>
    [[nodiscard]] predicate not_between(Lower &&lower, Upper &&upper) const {
      return lt(std::forward<Lower>(lower)) || gt(std::forward<Upper>(upper));
    }

    template <typename First, typename... Rest,
              std::enable_if_t<detail::is_compatible_value_v<T, First> &&
                                   (detail::is_compatible_value_v<T, Rest> && ...),
                               int> = 0>
    [[nodiscard]] predicate in(First &&first, Rest &&...rest) const {
      predicate output = eq(std::forward<First>(first));
      ((output = std::move(output) || eq(std::forward<Rest>(rest))), ...);
      return output;
    }

    template <typename First, typename... Rest,
              std::enable_if_t<detail::is_compatible_value_v<T, First> &&
                                   (detail::is_compatible_value_v<T, Rest> && ...),
                               int> = 0>
    [[nodiscard]] predicate not_in(First &&first, Rest &&...rest) const {
      predicate output = ne(std::forward<First>(first));
      ((output = std::move(output) && ne(std::forward<Rest>(rest))), ...);
      return output;
    }

    template <typename Other, std::enable_if_t<detail::is_compatible_column_v<T, Other>, int> = 0>
    [[nodiscard]] predicate eq_column(const column<Other> &other) const {
      return compare_column(comparison::equal, other);
    }

    template <typename Other, std::enable_if_t<detail::is_compatible_column_v<T, Other>, int> = 0>
    [[nodiscard]] predicate ne_column(const column<Other> &other) const {
      return compare_column(comparison::not_equal, other);
    }

    template <typename Other, std::enable_if_t<detail::is_compatible_column_v<T, Other>, int> = 0>
    [[nodiscard]] predicate lt_column(const column<Other> &other) const {
      return compare_column(comparison::less, other);
    }

    template <typename Other, std::enable_if_t<detail::is_compatible_column_v<T, Other>, int> = 0>
    [[nodiscard]] predicate le_column(const column<Other> &other) const {
      return compare_column(comparison::less_equal, other);
    }

    template <typename Other, std::enable_if_t<detail::is_compatible_column_v<T, Other>, int> = 0>
    [[nodiscard]] predicate gt_column(const column<Other> &other) const {
      return compare_column(comparison::greater, other);
    }

    template <typename Other, std::enable_if_t<detail::is_compatible_column_v<T, Other>, int> = 0>
    [[nodiscard]] predicate ge_column(const column<Other> &other) const {
      return compare_column(comparison::greater_equal, other);
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate operator==(Input &&input) const {
      return eq(std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate operator!=(Input &&input) const {
      return ne(std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate operator<(Input &&input) const {
      return lt(std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate operator<=(Input &&input) const {
      return le(std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate operator>(Input &&input) const {
      return gt(std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate operator>=(Input &&input) const {
      return ge(std::forward<Input>(input));
    }

    template <typename Other, std::enable_if_t<detail::is_compatible_column_v<T, Other>, int> = 0>
    [[nodiscard]] join_condition operator==(const column<Other> &other) const {
      return join(comparison::equal, other);
    }

    template <typename Other, std::enable_if_t<detail::is_compatible_column_v<T, Other>, int> = 0>
    [[nodiscard]] join_condition operator!=(const column<Other> &other) const {
      return join(comparison::not_equal, other);
    }

    template <typename Other, std::enable_if_t<detail::is_compatible_column_v<T, Other>, int> = 0>
    [[nodiscard]] join_condition operator<(const column<Other> &other) const {
      return join(comparison::less, other);
    }

    template <typename Other, std::enable_if_t<detail::is_compatible_column_v<T, Other>, int> = 0>
    [[nodiscard]] join_condition operator<=(const column<Other> &other) const {
      return join(comparison::less_equal, other);
    }

    template <typename Other, std::enable_if_t<detail::is_compatible_column_v<T, Other>, int> = 0>
    [[nodiscard]] join_condition operator>(const column<Other> &other) const {
      return join(comparison::greater, other);
    }

    template <typename Other, std::enable_if_t<detail::is_compatible_column_v<T, Other>, int> = 0>
    [[nodiscard]] join_condition operator>=(const column<Other> &other) const {
      return join(comparison::greater_equal, other);
    }

    [[nodiscard]] order_specifier asc() const {
      return order_specifier(qualified_, sort_order::ascending);
    }

    [[nodiscard]] order_specifier desc() const {
      return order_specifier(qualified_, sort_order::descending);
    }

  private:
    template <typename Input>
    [[nodiscard]] predicate compare(comparison operation, Input &&input) const {
      return predicate::condition(table_, qualified_, operation, value(std::forward<Input>(input)));
    }

    template <typename Other>
    [[nodiscard]] predicate compare_column(comparison operation,
                                           const column<Other> &other) const {
      return predicate::columns(table_, qualified_, operation, std::string(other.table_name()),
                                std::string(other.qualified_name()));
    }

    template <typename Other>
    [[nodiscard]] join_condition join(comparison operation, const column<Other> &other) const {
      return join_condition(table_, qualified_, operation, std::string(other.table_name()),
                            std::string(other.qualified_name()));
    }

    std::string table_;
    std::string name_;
    std::string qualified_;
  };

  template <typename T> class aggregate_expression final {
  public:
    using value_type = T;

    [[nodiscard]] aggregate_expression as(std::string alias) const {
      if (alias.empty()) throw std::invalid_argument("ORM aggregate alias is empty");
      auto result = *this;
      result.alias_ = std::move(alias);
      return result;
    }

    [[nodiscard]] aggregate_function function() const noexcept { return function_; }
    [[nodiscard]] std::string_view column_name() const noexcept { return column_; }
    [[nodiscard]] std::string_view alias() const noexcept { return alias_; }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate operator==(Input &&input) const {
      return compare(comparison::equal, std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate operator!=(Input &&input) const {
      return compare(comparison::not_equal, std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate operator<(Input &&input) const {
      return compare(comparison::less, std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate operator<=(Input &&input) const {
      return compare(comparison::less_equal, std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate operator>(Input &&input) const {
      return compare(comparison::greater, std::forward<Input>(input));
    }

    template <typename Input, std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    [[nodiscard]] predicate operator>=(Input &&input) const {
      return compare(comparison::greater_equal, std::forward<Input>(input));
    }

  private:
    friend struct detail::aggregate_expression_factory;
    template <typename U> friend aggregate_expression<std::uint64_t> count(const column<U> &);
    friend aggregate_expression<std::uint64_t> count_all();
    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> Enable>
    friend aggregate_expression<U> sum(const column<U> &);
    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> Enable>
    friend aggregate_expression<double> avg(const column<U> &);
    template <typename U> friend aggregate_expression<U> min(const column<U> &);
    template <typename U> friend aggregate_expression<U> max(const column<U> &);

    aggregate_expression(aggregate_function function, std::string column_name)
        : function_(function), column_(std::move(column_name)) {}

    template <typename Input>
    [[nodiscard]] predicate compare(comparison operation, Input &&input) const {
      return predicate::aggregate(function_, column_, operation, value(std::forward<Input>(input)));
    }

    aggregate_function function_;
    std::string column_;
    std::string alias_;
  };

  namespace detail {
    struct aggregate_expression_factory {
      template <typename T>
      static aggregate_expression<T> make(aggregate_function function,
                                          std::string column_name) {
        return aggregate_expression<T>(function, std::move(column_name));
      }
    };

    template <typename T>
    template <typename Input>
    expression<T> scalar_expression_factory<T>::from_value(Input input) {
      return expression<T>(std::vector<scalar_token_spec>{
          {ORM_SCALAR_VALUE, {}, value(std::move(input))}},
                           {});
    }

    template <typename T>
    expression<T> scalar_expression_factory<T>::from_tokens(
        std::vector<scalar_token_spec> tokens, std::string alias) {
      return expression<T>(std::move(tokens), std::move(alias));
    }
  } // namespace detail

  template <typename T> class expression final {
  public:
    using value_type = T;

    explicit expression(const column<T> &input)
        : tokens_{{ORM_SCALAR_COLUMN, std::string(input.qualified_name()), value(nullptr)}} {}

    expression as(std::string alias) const {
      expression copy = *this;
      copy.alias_ = std::move(alias);
      return copy;
    }

    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> = 0>
    auto operator+(U input) const { return append<U>(ORM_SCALAR_ADD, input); }
    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> = 0>
    auto operator-(U input) const { return append<U>(ORM_SCALAR_SUBTRACT, input); }
    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> = 0>
    auto operator*(U input) const { return append<U>(ORM_SCALAR_MULTIPLY, input); }
    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> = 0>
    auto operator/(U input) const { return append<U>(ORM_SCALAR_DIVIDE, input); }

    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> = 0>
    auto operator+(const column<U> &input) const {
      return combine<U>(ORM_SCALAR_ADD, expression<U>(input));
    }
    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> = 0>
    auto operator-(const column<U> &input) const {
      return combine<U>(ORM_SCALAR_SUBTRACT, expression<U>(input));
    }
    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> = 0>
    auto operator*(const column<U> &input) const {
      return combine<U>(ORM_SCALAR_MULTIPLY, expression<U>(input));
    }
    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> = 0>
    auto operator/(const column<U> &input) const {
      return combine<U>(ORM_SCALAR_DIVIDE, expression<U>(input));
    }

    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> = 0>
    auto operator+(const expression<U> &input) const {
      return combine<U>(ORM_SCALAR_ADD, input);
    }
    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> = 0>
    auto operator-(const expression<U> &input) const {
      return combine<U>(ORM_SCALAR_SUBTRACT, input);
    }
    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> = 0>
    auto operator*(const expression<U> &input) const {
      return combine<U>(ORM_SCALAR_MULTIPLY, input);
    }
    template <typename U, std::enable_if_t<detail::is_number_v<U>, int> = 0>
    auto operator/(const expression<U> &input) const {
      return combine<U>(ORM_SCALAR_DIVIDE, input);
    }

    [[nodiscard]] const std::vector<detail::scalar_token_spec> &tokens() const noexcept {
      return tokens_;
    }

    [[nodiscard]] std::string_view alias() const noexcept { return alias_; }

  private:
    template <typename> friend class expression;
    template <typename> friend struct detail::scalar_expression_factory;

    template <typename U> using result_t = std::common_type_t<detail::scalar_type_t<T>,
                                                               detail::scalar_type_t<U>>;
    template <typename U>
    expression<result_t<U>> combine(orm_scalar_token_kind_t kind,
                                    const expression<U> &input) const {
      std::vector<detail::scalar_token_spec> tokens = tokens_;
      tokens.reserve(tokens_.size() + input.tokens_.size() + 1);
      tokens.insert(tokens.end(), input.tokens_.begin(), input.tokens_.end());
      tokens.push_back({kind, {}, value(nullptr)});
      return detail::scalar_expression_factory<result_t<U>>::from_tokens(
          std::move(tokens), alias_);
    }

    template <typename U> expression<result_t<U>> append(orm_scalar_token_kind_t kind,
                                                        U input) const {
      std::vector<detail::scalar_token_spec> tokens = tokens_;
      tokens.reserve(tokens_.size() + 2);
      tokens.push_back({ORM_SCALAR_VALUE, {}, value(std::move(input))});
      tokens.push_back({kind, {}, value(nullptr)});
      return detail::scalar_expression_factory<result_t<U>>::from_tokens(std::move(tokens),
                                                                       alias_);
    }

    expression(std::vector<detail::scalar_token_spec> tokens, std::string alias)
        : tokens_(std::move(tokens)), alias_(std::move(alias)) {}
    std::vector<detail::scalar_token_spec> tokens_;
    std::string alias_;
  };

  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator+(const column<T> &left, U right) { return expression<T>(left) + right; }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator-(const column<T> &left, U right) { return expression<T>(left) - right; }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator*(const column<T> &left, U right) { return expression<T>(left) * right; }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator/(const column<T> &left, U right) { return expression<T>(left) / right; }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator+(const column<T> &left, const column<U> &right) {
    return expression<T>(left) + expression<U>(right);
  }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator-(const column<T> &left, const column<U> &right) {
    return expression<T>(left) - expression<U>(right);
  }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator*(const column<T> &left, const column<U> &right) {
    return expression<T>(left) * expression<U>(right);
  }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator/(const column<T> &left, const column<U> &right) {
    return expression<T>(left) / expression<U>(right);
  }

  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator+(T left, const expression<U> &right) {
    using result_t = std::common_type_t<detail::scalar_type_t<T>, detail::scalar_type_t<U>>;
    return detail::scalar_expression_factory<result_t>::from_value(left) + right;
  }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator-(T left, const expression<U> &right) {
    using result_t = std::common_type_t<detail::scalar_type_t<T>, detail::scalar_type_t<U>>;
    return detail::scalar_expression_factory<result_t>::from_value(left) - right;
  }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator*(T left, const expression<U> &right) {
    using result_t = std::common_type_t<detail::scalar_type_t<T>, detail::scalar_type_t<U>>;
    return detail::scalar_expression_factory<result_t>::from_value(left) * right;
  }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator/(T left, const expression<U> &right) {
    using result_t = std::common_type_t<detail::scalar_type_t<T>, detail::scalar_type_t<U>>;
    return detail::scalar_expression_factory<result_t>::from_value(left) / right;
  }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator+(T left, const column<U> &right) {
    using result_t = std::common_type_t<detail::scalar_type_t<T>, detail::scalar_type_t<U>>;
    return detail::scalar_expression_factory<result_t>::from_value(left) + expression<U>(right);
  }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator-(T left, const column<U> &right) {
    using result_t = std::common_type_t<detail::scalar_type_t<T>, detail::scalar_type_t<U>>;
    return detail::scalar_expression_factory<result_t>::from_value(left) - expression<U>(right);
  }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator*(T left, const column<U> &right) {
    using result_t = std::common_type_t<detail::scalar_type_t<T>, detail::scalar_type_t<U>>;
    return detail::scalar_expression_factory<result_t>::from_value(left) * expression<U>(right);
  }
  template <typename T, typename U,
            std::enable_if_t<detail::is_number_v<T> && detail::is_number_v<U>, int> = 0>
  auto operator/(T left, const column<U> &right) {
    using result_t = std::common_type_t<detail::scalar_type_t<T>, detail::scalar_type_t<U>>;
    return detail::scalar_expression_factory<result_t>::from_value(left) / expression<U>(right);
  }

  template <typename T>
  [[nodiscard]] aggregate_expression<std::uint64_t> count(const column<T> &input) {
    return detail::aggregate_expression_factory::make<std::uint64_t>(
        aggregate_function::count, std::string(input.qualified_name()));
  }

  [[nodiscard]] inline aggregate_expression<std::uint64_t> count_all() {
    return detail::aggregate_expression_factory::make<std::uint64_t>(
        aggregate_function::count_all, {});
  }

  template <typename T, std::enable_if_t<detail::is_number_v<T>, int> = 0>
  [[nodiscard]] aggregate_expression<T> sum(const column<T> &input) {
    return detail::aggregate_expression_factory::make<T>(
        aggregate_function::sum, std::string(input.qualified_name()));
  }

  template <typename T, std::enable_if_t<detail::is_number_v<T>, int> = 0>
  [[nodiscard]] aggregate_expression<double> avg(const column<T> &input) {
    return detail::aggregate_expression_factory::make<double>(
        aggregate_function::average, std::string(input.qualified_name()));
  }

  template <typename T> [[nodiscard]] aggregate_expression<T> min(const column<T> &input) {
    return detail::aggregate_expression_factory::make<T>(
        aggregate_function::minimum, std::string(input.qualified_name()));
  }

  template <typename T> [[nodiscard]] aggregate_expression<T> max(const column<T> &input) {
    return detail::aggregate_expression_factory::make<T>(
        aggregate_function::maximum, std::string(input.qualified_name()));
  }

  namespace detail {
    struct projection_spec {
      enum class kind { column, aggregate, expression } type;
      std::string column;
      aggregate_function function = aggregate_function::count_all;
      std::string alias;
      std::vector<scalar_token_spec> tokens;
    };

    template <typename T> struct is_projection : std::false_type {};
    template <typename T> struct is_projection<column<T>> : std::true_type {};
    template <typename T> struct is_projection<aggregate_expression<T>> : std::true_type {};
    template <typename T> struct is_projection<expression<T>> : std::true_type {};

    template <typename T> struct projection_value;
    template <typename T> struct projection_value<column<T>> { using type = T; };
    template <typename T> struct projection_value<aggregate_expression<T>> { using type = T; };
    template <typename T> struct projection_value<expression<T>> { using type = T; };

    template <typename... Projections>
    using projection_row_t =
        std::tuple<typename projection_value<std::decay_t<Projections>>::type...>;

    template <typename Row> struct single_projection_value;
    template <typename T> struct single_projection_value<std::tuple<T>> { using type = T; };

    template <typename Row> struct subquery_projection {
      static constexpr bool valid = false;
    };
    template <typename T> struct subquery_projection<std::tuple<T>> {
      using type = T;
      static constexpr bool valid = true;
    };

    template <typename Row, typename Mapper, typename = void>
    struct projection_mapper {
      static constexpr bool valid = false;
    };

    template <typename... Fields, typename Mapper>
    struct projection_mapper<
        std::tuple<Fields...>, Mapper,
        std::void_t<std::invoke_result_t<std::decay_t<Mapper> &, Fields...>>> {
      using invocation_type = std::invoke_result_t<std::decay_t<Mapper> &, Fields...>;
      using result_type = std::decay_t<invocation_type>;
      static constexpr bool valid =
          !std::is_void_v<result_type> && std::is_move_constructible_v<result_type>;
    };

    template <typename T>
    inline constexpr bool is_projection_v = is_projection<std::decay_t<T>>::value;

    template <typename T> projection_spec projection_of(const column<T> &input) {
      return {projection_spec::kind::column,
              std::string(input.qualified_name()),
              aggregate_function::count_all,
              {}};
    }

    template <typename T> projection_spec projection_of(const aggregate_expression<T> &input) {
      return {projection_spec::kind::aggregate, std::string(input.column_name()), input.function(),
              std::string(input.alias())};
    }

    template <typename T> projection_spec projection_of(const expression<T> &input) {
      projection_spec output{};
      output.type = projection_spec::kind::expression;
      output.alias = std::string(input.alias());
      output.tokens = input.tokens();
      return output;
    }

  } // namespace detail

  class config final {
  public:
    explicit config(std::string driver) : driver_(std::move(driver)) {
      if (driver_.empty()) throw std::invalid_argument("ORM driver identifier is empty");
      orm_config(&native_);
    }

    config &option(std::string keyword, std::string option_value) {
      if (keyword.empty()) throw std::invalid_argument("ORM option keyword is empty");
      options_.emplace_back(std::move(keyword), std::move(option_value));
      return *this;
    }

    config &max_parameters(std::uint32_t value) noexcept {
      native_.max_parameters = value;
      return *this;
    }

    config &max_columns(std::uint32_t value) noexcept {
      native_.max_columns = value;
      return *this;
    }

    config &max_predicates(std::uint32_t value) noexcept {
      native_.max_predicates = value;
      return *this;
    }

    config &max_joins(std::uint32_t value) noexcept {
      native_.max_joins = value;
      return *this;
    }

    config &max_group_columns(std::uint32_t value) noexcept {
      native_.max_group_columns = value;
      return *this;
    }

    config &max_assignments(std::uint32_t value) noexcept {
      native_.max_assignments = value;
      return *this;
    }

    config &max_condition_depth(std::uint32_t value) noexcept {
      native_.max_condition_depth = value;
      return *this;
    }

    config &max_query_bytes(std::uint64_t value) noexcept {
      native_.max_query_bytes = value;
      return *this;
    }

    config &max_parameter_bytes(std::uint64_t value) noexcept {
      native_.max_parameter_bytes = value;
      return *this;
    }

    config &max_result_rows(std::uint64_t value) noexcept {
      native_.max_result_rows = value;
      return *this;
    }

    config &max_result_bytes(std::uint64_t value) noexcept {
      native_.max_result_bytes = value;
      return *this;
    }

  private:
    friend class connection;

    orm_config_t native_{};
    std::string driver_;
    std::vector<std::pair<std::string, std::string>> options_;
  };

  class result;
  class transaction;

  class query final {
  public:
    ~query() noexcept { orm_query_destroy(handle_); }

    query(const query &) = delete;
    query &operator=(const query &) = delete;

    query(query &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    query &operator=(query &&other) noexcept {
      if (this != &other) {
        orm_query_destroy(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
      }
      return *this;
    }

    query &select_all() {
      checked("select all",
              [&](orm_error_t *error) { return orm_query_select_all(handle_, error); });
      return *this;
    }

    query &distinct(bool enabled = true) {
      checked("set DISTINCT", [&](orm_error_t *error) {
        return orm_query_set_distinct(handle_, enabled ? 1 : 0, error);
      });
      return *this;
    }

    query &column(std::string_view name) {
      checked("add selected column", [&](orm_error_t *error) {
        return orm_query_add_column(handle_, detail::to_view(name), error);
      });
      return *this;
    }

    query &aggregate(aggregate_function function, std::string_view column_name = {},
                     std::string_view alias = {}) {
      checked("add aggregate", [&](orm_error_t *error) {
        return orm_query_add_aggregate(handle_, static_cast<orm_aggregate_t>(function),
                                       detail::to_view(column_name), detail::to_view(alias), error);
      });
      return *this;
    }

    query &scalar_expression(const std::vector<detail::scalar_token_spec> &tokens,
                             std::string_view alias = {}) {
      if (tokens.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("ORM scalar expression token count exceeds uint32_t");
      std::vector<orm_scalar_token_t> native;
      native.reserve(tokens.size());
      for (const auto &token : tokens)
        native.push_back({token.kind, detail::to_view(token.column), token.operand.native()});
      const orm_scalar_expression_t expression{native.data(),
                                               static_cast<std::uint32_t>(native.size())};
      checked("add scalar expression", [&](orm_error_t *error) {
        return orm_query_add_expression(handle_, expression, detail::to_view(alias), error);
      });
      return *this;
    }

    query &set(std::string_view column_name, value input) {
      checked("set assignment", [&](orm_error_t *error) {
        return orm_query_set(handle_, detail::to_view(column_name), input.native(), error);
      });
      return *this;
    }

    query &where(std::string_view column_name, comparison operation, value input) {
      checked("append WHERE predicate", [&](orm_error_t *error) {
        return orm_query_where(handle_, detail::to_view(column_name),
                               static_cast<orm_compare_t>(operation), input.native(), error);
      });
      return *this;
    }

    query &where_columns(std::string_view left_column, comparison operation,
                         std::string_view right_column) {
      checked("append column WHERE predicate", [&](orm_error_t *error) {
        return orm_query_where_columns(handle_, detail::to_view(left_column),
                                       static_cast<orm_compare_t>(operation),
                                       detail::to_view(right_column), error);
      });
      return *this;
    }

    query &where_exists(const query &subquery, bool negated = false) {
      checked(negated ? "append NOT EXISTS subquery" : "append EXISTS subquery",
              [&](orm_error_t *error) {
                return orm_query_where_exists(handle_, subquery.handle_, negated ? 1 : 0, error);
              });
      return *this;
    }

    query &where_in_subquery(std::string_view column_name, const query &subquery,
                             bool negated = false) {
      checked(negated ? "append NOT IN subquery" : "append IN subquery",
              [&](orm_error_t *error) {
                return orm_query_where_in_subquery(handle_, detail::to_view(column_name),
                                                   subquery.handle_, negated ? 1 : 0, error);
              });
      return *this;
    }

    query &where_scalar_subquery(std::string_view column_name, comparison operation,
                                 const query &subquery) {
      checked("append scalar subquery comparison", [&](orm_error_t *error) {
        return orm_query_where_scalar_subquery(
            handle_, detail::to_view(column_name), static_cast<orm_compare_t>(operation),
            subquery.handle_, error);
      });
      return *this;
    }

    query &where_quantified_subquery(std::string_view column_name,
                                     comparison operation,
                                     subquery_quantifier quantifier,
                                     const query &subquery) {
      checked("append quantified subquery comparison", [&](orm_error_t *error) {
        return orm_query_where_quantified_subquery(
            handle_, detail::to_view(column_name), static_cast<orm_compare_t>(operation),
            static_cast<orm_subquery_quantifier_t>(quantifier), subquery.handle_, error);
      });
      return *this;
    }

    query &begin_where(logic operation) {
      checked("begin WHERE group", [&](orm_error_t *error) {
        return orm_query_begin_where_group(handle_, static_cast<orm_logic_t>(operation), error);
      });
      return *this;
    }

    query &end_where() {
      checked("end WHERE group",
              [&](orm_error_t *error) { return orm_query_end_where_group(handle_, error); });
      return *this;
    }

    query &join(join_type type, std::string_view table, std::string_view left_column,
                comparison operation, std::string_view right_column) {
      checked("append JOIN", [&](orm_error_t *error) {
        return orm_query_join(handle_, static_cast<orm_join_t>(type), detail::to_view(table),
                              detail::to_view(left_column), static_cast<orm_compare_t>(operation),
                              detail::to_view(right_column), error);
      });
      return *this;
    }

    query &group_by(std::string_view column_name) {
      checked("append GROUP BY", [&](orm_error_t *error) {
        return orm_query_group_by(handle_, detail::to_view(column_name), error);
      });
      return *this;
    }

    query &having(std::string_view column_name, comparison operation, value input) {
      checked("append HAVING predicate", [&](orm_error_t *error) {
        return orm_query_having(handle_, detail::to_view(column_name),
                                static_cast<orm_compare_t>(operation), input.native(), error);
      });
      return *this;
    }

    query &having(aggregate_function function, std::string_view column_name, comparison operation,
                  value input) {
      checked("append aggregate HAVING predicate", [&](orm_error_t *error) {
        return orm_query_having_aggregate(
            handle_, static_cast<orm_aggregate_t>(function), detail::to_view(column_name),
            static_cast<orm_compare_t>(operation), input.native(), error);
      });
      return *this;
    }

    query &begin_having(logic operation) {
      checked("begin HAVING group", [&](orm_error_t *error) {
        return orm_query_begin_having_group(handle_, static_cast<orm_logic_t>(operation), error);
      });
      return *this;
    }

    query &end_having() {
      checked("end HAVING group",
              [&](orm_error_t *error) { return orm_query_end_having_group(handle_, error); });
      return *this;
    }

    query &bind(value input) {
      checked("bind raw parameter",
              [&](orm_error_t *error) { return orm_query_bind(handle_, input.native(), error); });
      return *this;
    }

    query &order_by(std::string_view column_name, sort_order order) {
      checked("append ORDER BY", [&](orm_error_t *error) {
        return orm_query_order_by(handle_, detail::to_view(column_name),
                                  static_cast<orm_order_t>(order), error);
      });
      return *this;
    }

    template <typename T>
    query &order_by(const expression<T> &input, sort_order order) {
      std::vector<orm_scalar_token_t> tokens;
      tokens.reserve(input.tokens().size());
      for (const auto &token : input.tokens()) {
        tokens.push_back(
            {token.kind, detail::to_view(token.column), token.operand.native()});
      }
      const orm_scalar_expression_t expression{tokens.data(),
                                              static_cast<std::uint32_t>(tokens.size())};
      checked("append ORDER BY scalar expression", [&](orm_error_t *error) {
        return orm_query_order_by_expression(handle_, expression,
                                            static_cast<orm_order_t>(order), error);
      });
      return *this;
    }

    query &limit(std::uint64_t count) {
      checked("set LIMIT",
              [&](orm_error_t *error) { return orm_query_set_limit(handle_, count, error); });
      return *this;
    }

    query &offset(std::uint64_t count) {
      checked("set OFFSET",
              [&](orm_error_t *error) { return orm_query_set_offset(handle_, count, error); });
      return *this;
    }

    [[nodiscard]] result execute();
    [[nodiscard]] result execute(transaction &owner);

    template <typename T> [[nodiscard]] std::vector<T> fetch();
    template <typename T> [[nodiscard]] std::vector<T> fetch(transaction &owner);

  private:
    friend class connection;

    explicit query(orm_query_t *handle) noexcept : handle_(handle) {}

    template <typename Operation>
    void checked(std::string_view operation, Operation &&invoke) const {
      if (handle_ == nullptr) throw std::logic_error("ORM query has no handle");
      detail::check(operation, std::forward<Operation>(invoke));
    }

    orm_query_t *handle_ = nullptr;
  };

  class exists_predicate final {
  public:
    exists_predicate(const exists_predicate &) = delete;
    exists_predicate &operator=(const exists_predicate &) = delete;
    exists_predicate(exists_predicate &&) noexcept = default;
    exists_predicate &operator=(exists_predicate &&) noexcept = default;

    void apply(query &outer) const { outer.where_exists(subquery_, negated_); }

  private:
    friend class select_query;
    template <typename> friend class typed_select_query;

    exists_predicate(query subquery, bool negated)
        : subquery_(std::move(subquery)), negated_(negated) {}

    query subquery_;
    bool negated_;
  };

  namespace detail {

    template <typename T> struct is_optional : std::false_type {};

    template <typename T> struct is_optional<std::optional<T>> : std::true_type {
      using value_type = T;
    };

    template <typename T> struct is_blob_vector : std::false_type {};

    template <> struct is_blob_vector<std::vector<std::uint8_t>> : std::true_type {};

    template <typename T>
    struct is_fetch_record
        : std::bool_constant<model::is_entity<T>::value ||
                             model::is_tuple<T>::value> {};

    template <typename> struct always_false : std::false_type {};

    template <typename T>
    inline constexpr bool has_model_v =
        model::is_entity<std::remove_cv_t<std::remove_reference_t<T>>>::value;

    template <typename T, typename Visitor>
    void for_each_flat_field(const T &object, Visitor &&visitor) {
      static_assert(has_model_v<T>,
                    "ORM modeled field traversal requires an entity model");
      model::for_each(object, [&](auto member, auto field_name, auto) {
        using field_type =
            std::remove_cv_t<std::remove_reference_t<decltype(object.*member)>>;
        if constexpr (has_model_v<field_type>) {
          for_each_flat_field(object.*member, visitor);
        } else {
          visitor(std::string_view(field_name), object.*member);
        }
      });
    }

    template <typename T, typename Visitor>
    void for_each_flat_field_mutable(T &object, Visitor &&visitor) {
      static_assert(has_model_v<T>,
                    "ORM mutable modeled field traversal requires an entity model");
      model::for_each(object, [&](auto member, auto field_name, auto) {
        using field_type =
            std::remove_cv_t<std::remove_reference_t<decltype(object.*member)>>;
        if constexpr (has_model_v<field_type>) {
          for_each_flat_field_mutable(object.*member, visitor);
        } else {
          visitor(std::string_view(field_name), object.*member);
        }
      });
    }

    template <typename T> void copy_modeled_fields(T &target, const T &source) {
      static_assert(has_model_v<T>,
                    "ORM modeled field copy requires an entity model");
      model::for_each(target, [&](auto member, auto, auto) {
        using field_type =
            std::remove_cv_t<std::remove_reference_t<decltype(target.*member)>>;
        if constexpr (has_model_v<field_type>) {
          copy_modeled_fields(target.*member, source.*member);
        } else {
          target.*member = source.*member;
        }
      });
    }

    template <typename T, typename Visitor>
    void for_each_flat_field_name(Visitor &&visitor) {
      using value_type = std::remove_cv_t<std::remove_reference_t<T>>;
      static_assert(has_model_v<value_type>,
                    "ORM automatic projection requires an entity model");
      static_assert(std::is_default_constructible_v<value_type>,
                    "ORM automatic projection requires a default-constructible struct");
      value_type sample{};
      for_each_flat_field(sample, [&](std::string_view name, const auto &) { visitor(name); });
    }

    template <typename T> value model_value(const T &input) {
      using value_type = std::remove_cv_t<std::remove_reference_t<T>>;
      if constexpr (is_optional<value_type>::value) {
        if (!input.has_value()) return value(nullptr);
        return model_value(*input);
      } else if constexpr (std::is_array_v<value_type>) {
        using element_type = std::remove_cv_t<std::remove_extent_t<value_type>>;
        static_assert(std::is_same_v<element_type, char>,
                      "ORM modeled array values must be fixed-size character arrays");
        constexpr std::size_t capacity = std::extent_v<value_type>;
        std::size_t length = 0;
        while (length < capacity && input[length] != '\0') ++length;
        return value(std::string_view(input, length));
      } else {
        static_assert(std::is_constructible_v<value, const T &>,
                      "ORM modeled field cannot be converted to an ORM value");
        return value(input);
      }
    }

    template <typename T> bool model_equal(const T &left, const T &right) {
      using value_type = std::remove_cv_t<std::remove_reference_t<T>>;
      if constexpr (is_optional<value_type>::value) {
        if (left.has_value() != right.has_value()) return false;
        return !left.has_value() || model_equal(*left, *right);
      } else if constexpr (std::is_array_v<value_type>) {
        using element_type = std::remove_cv_t<std::remove_extent_t<value_type>>;
        static_assert(std::is_same_v<element_type, char>,
                      "ORM dirty checking only supports fixed-size character arrays");
        constexpr std::size_t count = std::extent_v<value_type>;
        for (std::size_t index = 0; index < count; ++index)
          if (left[index] != right[index]) return false;
        return true;
      } else if constexpr (has_model_v<value_type>) {
        bool equal = true;
        model::for_each(left, [&](auto member, auto, auto) {
          if (equal) equal = model_equal(left.*member, right.*member);
        });
        return equal;
      } else if constexpr (is_blob_vector<value_type>::value ||
                           std::is_arithmetic_v<value_type> ||
                           std::is_enum_v<value_type> ||
                           std::is_same_v<value_type, std::string>) {
        return left == right;
      } else {
        static_assert(always_false<value_type>::value,
                      "ORM dirty checking does not support this modeled field type");
      }
    }

    template <typename T> std::string identity_component(const T &input) {
      using value_type = std::remove_cv_t<std::remove_reference_t<T>>;
      if constexpr (is_optional<value_type>::value) {
        if (!input.has_value())
          throw std::invalid_argument("ORM identity-map primary-key value cannot be NULL");
        return identity_component(*input);
      } else if constexpr (std::is_array_v<value_type>) {
        using element_type = std::remove_cv_t<std::remove_extent_t<value_type>>;
        static_assert(std::is_same_v<element_type, char>,
                      "ORM identity-map keys only support fixed-size character arrays");
        constexpr std::size_t capacity = std::extent_v<value_type>;
        std::size_t length = 0;
        while (length < capacity && input[length] != '\0') ++length;
        return "s:" + std::to_string(length) + ":" + std::string(input, length);
      } else if constexpr (std::is_same_v<value_type, std::string>) {
        return "s:" + std::to_string(input.size()) + ":" + input;
      } else if constexpr (std::is_same_v<value_type, std::string_view>) {
        return "s:" + std::to_string(input.size()) + ":" + std::string(input);
      } else if constexpr (std::is_same_v<value_type, const char *> ||
                           std::is_same_v<value_type, char *>) {
        if (input == nullptr)
          throw std::invalid_argument("ORM identity-map text primary-key value is null");
        return identity_component(std::string_view(input));
      } else if constexpr (std::is_same_v<value_type, bool>) {
        return std::string("b:") + (input ? "1" : "0");
      } else if constexpr (std::is_enum_v<value_type>) {
        return "e:" + identity_component(static_cast<std::underlying_type_t<value_type>>(input));
      } else if constexpr (std::is_integral_v<value_type> && std::is_signed_v<value_type>) {
        return "i:" + std::to_string(static_cast<long long>(input));
      } else if constexpr (std::is_integral_v<value_type> && std::is_unsigned_v<value_type>) {
        return "u:" + std::to_string(static_cast<unsigned long long>(input));
      } else {
        static_assert(always_false<value_type>::value,
                      "ORM identity-map keys require an integral, enum, string, or char array id");
      }
    }

    template <typename T> struct is_identity_component_type
        : std::bool_constant<std::is_integral_v<T> || std::is_enum_v<T> ||
                             std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> ||
                             std::is_same_v<T, const char *> || std::is_same_v<T, char *>> {};

    template <typename T> struct is_identity_component_type<std::optional<T>>
        : is_identity_component_type<std::remove_cv_t<std::remove_reference_t<T>>> {};

    template <typename T, std::size_t N>
    struct is_identity_component_type<T[N]> : std::is_same<std::remove_cv_t<T>, char> {};

    template <typename Entity, typename Id, typename Visitor>
    void for_each_primary_key_input(const Id &id, Visitor &&visitor) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      using id_type = std::remove_cv_t<std::remove_reference_t<Id>>;
      const auto names = model::get_primary_keys<entity_type>();
      if constexpr (is_optional<id_type>::value) {
        if (!id.has_value())
          throw std::invalid_argument("ORM primary-key value cannot be NULL");
        for_each_primary_key_input<entity_type>(*id,
                                                std::forward<Visitor>(visitor));
        return;
      } else {
        std::size_t index = 0;
        auto emit = [&](const auto &component) {
          if (index >= names.size())
            throw std::invalid_argument(
                "ORM composite id has too many components");
          using component_type =
              std::remove_cv_t<std::remove_reference_t<decltype(component)>>;
          if constexpr (!std::is_same_v<component_type, value>)
            (void)identity_component(component);
          visitor(names[index], component);
          ++index;
        };
        if constexpr (model::is_tuple<id_type>::value) {
          std::apply([&](const auto &...components) { (emit(components), ...); }, id);
        } else if constexpr (has_model_v<id_type>) {
          for_each_flat_field(id, [&](std::string_view name, const auto &component) {
            if (index >= names.size() || name != names[index])
              throw std::invalid_argument(
                  "ORM embedded id columns do not match entity primary-key metadata");
            emit(component);
          });
        } else {
          emit(id);
        }
        if (index != names.size())
          throw std::invalid_argument(
              "ORM composite id component count does not match entity metadata");
      }
    }

    struct identity_key final {
      std::type_index entity_type;
      std::vector<std::string> primary_key;

      friend bool operator==(const identity_key &left, const identity_key &right) noexcept {
        return left.entity_type == right.entity_type && left.primary_key == right.primary_key;
      }
    };

    struct identity_key_hash final {
      std::size_t operator()(const identity_key &key) const noexcept {
        std::size_t output = key.entity_type.hash_code();
        for (const std::string &component : key.primary_key) {
          const std::size_t component_hash = std::hash<std::string>{}(component);
          output ^= component_hash + static_cast<std::size_t>(0x9e3779b9) +
                    (output << 6) + (output >> 2);
        }
        return output;
      }
    };

    template <typename Entity>
    identity_key identity_key_for_entity(const Entity &entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      const auto primary_key_names = model::get_primary_keys<entity_type>();
      std::vector<std::optional<std::string>> components(primary_key_names.size());
      for_each_flat_field(entity, [&](std::string_view name, const auto &field) {
        const auto found = std::find(primary_key_names.begin(),
                                     primary_key_names.end(), name);
        if (found == primary_key_names.end()) return;
        const std::size_t index = static_cast<std::size_t>(
            std::distance(primary_key_names.begin(), found));
        if (components[index].has_value())
          throw std::invalid_argument(
              "ORM session entity has duplicate modeled primary-key fields");
        using field_type = std::remove_cv_t<std::remove_reference_t<decltype(field)>>;
        if constexpr (is_identity_component_type<field_type>::value) {
          components[index] = identity_component(field);
        } else {
          throw std::invalid_argument(
              "ORM session primary-key field has an unsupported modeled type");
        }
      });
      if (components.empty())
        throw std::invalid_argument("ORM session entity has no modeled primary-key field");
      std::vector<std::string> primary_key;
      primary_key.reserve(components.size());
      for (auto &component : components) {
        if (!component.has_value())
          throw std::invalid_argument(
              "ORM session entity is missing a modeled primary-key component");
        primary_key.push_back(std::move(*component));
      }
      return {std::type_index(typeid(entity_type)), std::move(primary_key)};
    }

    template <typename Entity> value primary_key_value(const Entity &entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      const std::string_view primary_key_name = model::get_primary_key<entity_type>();
      std::optional<value> primary_key;
      std::size_t matches = 0;
      for_each_flat_field(entity, [&](std::string_view name, const auto &field) {
        if (name != primary_key_name) return;
        ++matches;
        primary_key = model_value(field);
      });
      if (matches == 0)
        throw std::invalid_argument("ORM entity has no modeled primary-key field");
      if (matches > 1)
        throw std::invalid_argument("ORM entity has multiple modeled primary-key fields");
      if (!primary_key.has_value())
        throw std::invalid_argument("ORM entity primary-key field cannot be NULL");
      return std::move(*primary_key);
    }

    template <typename Entity, typename = void>
    struct has_model_id : std::false_type {};

    template <typename Entity>
    struct has_model_id<
        Entity,
        std::void_t<decltype(model::entity_model<Entity>::id(
            std::declval<const Entity &>()))>> : std::true_type {};

    template <typename Entity> decltype(auto) primary_key_id(const Entity &entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      if constexpr (has_model_id<entity_type>::value) {
        return model::entity_model<entity_type>::id(entity);
      } else {
        return primary_key_value(entity);
      }
    }

    template <typename Entity>
    inline constexpr bool is_versioned_entity_v =
        !model::get_version<
            std::remove_cv_t<std::remove_reference_t<Entity>>>().empty();

    template <typename Entity> value version_value(const Entity &entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      const std::string_view version_name = model::get_version<entity_type>();
      if (version_name.empty())
        throw std::invalid_argument("ORM entity has no modeled version field");
      std::optional<value> version;
      std::size_t matches = 0;
      for_each_flat_field(entity, [&](std::string_view name, const auto &field) {
        if (name != version_name) return;
        ++matches;
        using field_type = std::remove_cv_t<std::remove_reference_t<decltype(field)>>;
        if constexpr (std::is_integral_v<field_type> &&
                      !std::is_same_v<field_type, bool>) {
          version = model_value(field);
        } else {
          throw std::invalid_argument(
              "ORM version field must use a non-boolean integral type");
        }
      });
      if (matches != 1 || !version.has_value())
        throw std::invalid_argument(
            "ORM entity must expose exactly one modeled version field");
      return std::move(*version);
    }

    template <typename Entity> std::string version_token(const Entity &entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      const std::string_view version_name = model::get_version<entity_type>();
      std::optional<std::string> token;
      std::size_t matches = 0;
      for_each_flat_field(entity, [&](std::string_view name, const auto &field) {
        if (name != version_name) return;
        ++matches;
        using field_type = std::remove_cv_t<std::remove_reference_t<decltype(field)>>;
        if constexpr (std::is_integral_v<field_type> &&
                      !std::is_same_v<field_type, bool>) {
          token = identity_component(field);
        } else {
          throw std::invalid_argument(
              "ORM version field must use a non-boolean integral type");
        }
      });
      if (matches != 1 || !token.has_value())
        throw std::invalid_argument(
            "ORM entity must expose exactly one modeled version field");
      return std::move(*token);
    }

    template <typename Entity> void increment_version(Entity &entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      const std::string_view version_name = model::get_version<entity_type>();
      std::size_t matches = 0;
      for_each_flat_field_mutable(entity, [&](std::string_view name, auto &field) {
        if (name != version_name) return;
        ++matches;
        using field_type = std::remove_cv_t<std::remove_reference_t<decltype(field)>>;
        if constexpr (std::is_integral_v<field_type> &&
                      !std::is_same_v<field_type, bool>) {
          if (field == std::numeric_limits<field_type>::max())
            throw status_error(ORM_STATUS_OUT_OF_RANGE,
                               "ORM entity version overflow");
          ++field;
        } else {
          throw std::invalid_argument(
              "ORM version field must use a non-boolean integral type");
        }
      });
      if (matches != 1)
        throw std::invalid_argument(
            "ORM entity must expose exactly one modeled version field");
    }

    template <typename Entity> void decrement_version_noexcept(Entity &entity) noexcept {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      const std::string_view version_name = model::get_version<entity_type>();
      for_each_flat_field_mutable(entity, [&](std::string_view name, auto &field) {
        if (name != version_name) return;
        using field_type = std::remove_cv_t<std::remove_reference_t<decltype(field)>>;
        if constexpr (std::is_integral_v<field_type> &&
                      !std::is_same_v<field_type, bool>) {
          --field;
        }
      });
    }

    template <typename Entity, typename Id>
    identity_key identity_key_for_id(const Id &id) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      std::vector<std::string> components;
      components.reserve(model::get_primary_keys<entity_type>().size());
      for_each_primary_key_input<entity_type>(
          id, [&](std::string_view, const auto &component) {
            components.push_back(identity_component(component));
          });
      return {std::type_index(typeid(entity_type)), std::move(components)};
    }

    struct predicate_access {
      [[nodiscard]] static const std::shared_ptr<const predicate_node> &
      root(const predicate &input) noexcept {
        return input.root_;
      }
    };

    inline void apply_predicate_node(query &target, const predicate_node &node, bool having) {
      if (const auto *condition = std::get_if<condition_leaf>(&node.data)) {
        if (having) target.having(condition->column, condition->operation, condition->operand);
        else target.where(condition->column, condition->operation, condition->operand);
        return;
      }

      if (const auto *condition = std::get_if<column_condition_leaf>(&node.data)) {
        if (having)
          throw std::invalid_argument("ORM column comparison is not supported in HAVING");
        target.where_columns(condition->left_column, condition->operation,
                             condition->right_column);
        return;
      }

      if (const auto *aggregate = std::get_if<aggregate_leaf>(&node.data)) {
        if (!having) throw std::invalid_argument("ORM aggregate predicate is only valid in HAVING");
        target.having(aggregate->function, aggregate->column, aggregate->operation,
                      aggregate->operand);
        return;
      }

      const auto &group = std::get<condition_group>(node.data);
      if (having) target.begin_having(group.operation);
      else target.begin_where(group.operation);
      for (const auto &child : group.children)
        apply_predicate_node(target, *child, having);
      if (having) target.end_having();
      else target.end_where();
    }

    inline bool contains_aggregate(const predicate_node &node) {
      if (std::holds_alternative<aggregate_leaf>(node.data)) return true;
      const auto *group = std::get_if<condition_group>(&node.data);
      if (group == nullptr) return false;
      return std::any_of(group->children.begin(), group->children.end(),
                         [](const auto &child) { return contains_aggregate(*child); });
    }

    inline bool references_only_table(const predicate_node &node, std::string_view table_name) {
      if (const auto *condition = std::get_if<condition_leaf>(&node.data))
        return condition->table == table_name;
      if (const auto *condition = std::get_if<column_condition_leaf>(&node.data))
        return condition->left_table == table_name && condition->right_table == table_name;
      if (std::holds_alternative<aggregate_leaf>(node.data)) return false;
      const auto &group = std::get<condition_group>(node.data);
      return std::all_of(group.children.begin(), group.children.end(), [&](const auto &child) {
        return references_only_table(*child, table_name);
      });
    }

    inline void apply_where(query &target, const predicate &input,
                            std::string_view expected_table = {}) {
      const auto &root = predicate_access::root(input);
      if (root == nullptr) throw std::invalid_argument("ORM WHERE predicate is empty");
      if (contains_aggregate(*root))
        throw std::invalid_argument("ORM aggregate predicate is only valid in HAVING");
      if (!expected_table.empty() && !references_only_table(*root, expected_table))
        throw std::invalid_argument("ORM predicate column belongs to a different table");
      apply_predicate_node(target, *root, false);
    }

    inline void apply_having(query &target, const predicate &input) {
      const auto &root = predicate_access::root(input);
      if (root == nullptr) throw std::invalid_argument("ORM HAVING predicate is empty");
      apply_predicate_node(target, *root, true);
    }

  } // namespace detail

  class result final {
  public:
    ~result() noexcept { orm_result_destroy(handle_); }

    result(const result &) = delete;
    result &operator=(const result &) = delete;

    result(result &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    result &operator=(result &&other) noexcept {
      if (this != &other) {
        orm_result_destroy(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
      }
      return *this;
    }

    [[nodiscard]] std::uint64_t rows() const {
      std::uint64_t count = 0;
      checked("read result row count",
              [&](orm_error_t *error) { return orm_result_row_count(handle_, &count, error); });
      return count;
    }

    [[nodiscard]] std::uint64_t columns() const {
      std::uint64_t count = 0;
      checked("read result column count",
              [&](orm_error_t *error) { return orm_result_column_count(handle_, &count, error); });
      return count;
    }

    [[nodiscard]] std::uint64_t affected_rows() const {
      std::uint64_t count = 0;
      checked("read affected row count",
              [&](orm_error_t *error) { return orm_result_affected_rows(handle_, &count, error); });
      return count;
    }

    [[nodiscard]] bool is_null(std::uint64_t row, std::uint64_t column) const {
      std::uint8_t output = 0;
      checked("read result null state", [&](orm_error_t *error) {
        return orm_result_is_null(handle_, row, column, &output, error);
      });
      return output != 0;
    }

    [[nodiscard]] std::string text(std::uint64_t row, std::uint64_t column) const {
      orm_string_view_t output{};
      checked("read text result", [&](orm_error_t *error) {
        return orm_result_get_text(handle_, row, column, &output, error);
      });
      if (output.len == 0) return {};
      if (output.data == nullptr)
        throw status_error(ORM_STATUS_INTERNAL_ERROR,
                           "read text result: backend returned an invalid view");
      return std::string(output.data, output.len);
    }

    [[nodiscard]] std::vector<std::uint8_t> blob(std::uint64_t row,
                                                  std::uint64_t column) const {
      orm_blob_t output{};
      checked("read blob result", [&](orm_error_t *error) {
        return orm_result_get_blob(handle_, row, column, &output, error);
      });
      if (output.size == 0) return {};
      if (output.data == nullptr)
        throw status_error(ORM_STATUS_INTERNAL_ERROR,
                           "read blob result: backend returned an invalid view");
      const auto *begin = static_cast<const std::uint8_t *>(output.data);
      return std::vector<std::uint8_t>(begin, begin + output.size);
    }

    [[nodiscard]] std::int64_t int64(std::uint64_t row, std::uint64_t column) const {
      std::int64_t output = 0;
      checked("read signed integer result", [&](orm_error_t *error) {
        return orm_result_get_int64(handle_, row, column, &output, error);
      });
      return output;
    }

    [[nodiscard]] std::uint64_t uint64(std::uint64_t row, std::uint64_t column) const {
      std::uint64_t output = 0;
      checked("read unsigned integer result", [&](orm_error_t *error) {
        return orm_result_get_uint64(handle_, row, column, &output, error);
      });
      return output;
    }

    [[nodiscard]] double real(std::uint64_t row, std::uint64_t column) const {
      double output = 0.0;
      checked("read floating-point result", [&](orm_error_t *error) {
        return orm_result_get_double(handle_, row, column, &output, error);
      });
      return output;
    }

    [[nodiscard]] bool boolean(std::uint64_t row, std::uint64_t column) const {
      std::uint8_t output = 0;
      checked("read boolean result", [&](orm_error_t *error) {
        return orm_result_get_boolean(handle_, row, column, &output, error);
      });
      return output != 0;
    }

    /*
     * Materialize one projection row into a modeled value. Projection
     * order is the contract: the first selected column maps to the first
     * model member, and nested model members are flattened in order.
     */
    template <typename T> [[nodiscard]] T row(std::uint64_t row_index) const {
      using value_type = std::remove_cv_t<std::remove_reference_t<T>>;
      static_assert(std::is_same_v<T, value_type>,
                    "ORM result::row<T> requires an unqualified value type");
      static_assert(detail::is_fetch_record<value_type>::value,
                    "ORM result::row<T> requires an entity model or std::tuple");
      static_assert(std::is_default_constructible_v<value_type>,
                    "ORM result::row<T> requires a default-constructible destination");
      const std::size_t expected = mapped_column_count<value_type>();
      const std::uint64_t actual = columns();
      if (actual != expected)
        throw status_error(ORM_STATUS_TYPE_ERROR,
                           "map result row: projection column count does not match entity model");
      value_type output{};
      std::uint64_t column_index = 0;
      assign_record(output, row_index, column_index);
      if (column_index != actual)
        throw status_error(ORM_STATUS_INTERNAL_ERROR,
                           "map result row: entity model consumed an invalid number of columns");
      return output;
    }

    template <typename T> [[nodiscard]] std::vector<T> as() const {
      using value_type = std::remove_cv_t<std::remove_reference_t<T>>;
      static_assert(std::is_same_v<T, value_type>,
                    "ORM result::as<T> requires an unqualified value type");
      static_assert(detail::is_fetch_record<value_type>::value,
                    "ORM result::as<T> requires an entity model or std::tuple");
      static_assert(std::is_default_constructible_v<value_type>,
                    "ORM result::as<T> requires a default-constructible destination");
      const std::size_t expected = mapped_column_count<value_type>();
      const std::uint64_t actual = columns();
      if (actual != expected)
        throw status_error(ORM_STATUS_TYPE_ERROR,
                           "map result: projection column count does not match entity model");

      const std::uint64_t count = rows();
      std::vector<T> output;
      if (count > static_cast<std::uint64_t>(output.max_size()))
        throw status_error(ORM_STATUS_LIMIT_EXCEEDED,
                           "map result: row count exceeds the destination vector capacity");
      output.reserve(static_cast<std::size_t>(count));
      for (std::uint64_t row_index = 0; row_index < count; ++row_index) {
        value_type value{};
        std::uint64_t column_index = 0;
        assign_record(value, row_index, column_index);
        if (column_index != actual)
          throw status_error(ORM_STATUS_INTERNAL_ERROR,
                             "map result row: entity model consumed an invalid number of columns");
        output.push_back(std::move(value));
      }
      return output;
    }

  private:
    friend class query;

    explicit result(orm_result_t *handle) noexcept : handle_(handle) {}

    template <typename Operation>
    void checked(std::string_view operation, Operation &&invoke) const {
      if (handle_ == nullptr) throw std::logic_error("ORM result has no handle");
      detail::check(operation, std::forward<Operation>(invoke));
    }

    template <typename T>
    static constexpr bool has_model_v =
        model::is_entity<std::remove_cv_t<std::remove_reference_t<T>>>::value;

    template <typename T>
    static std::size_t mapped_column_count() {
      using value_type = std::remove_cv_t<std::remove_reference_t<T>>;
      if constexpr (detail::is_optional<value_type>::value) {
        using nested_type = typename detail::is_optional<value_type>::value_type;
        static_assert(!detail::is_fetch_record<nested_type>::value,
                      "ORM typed fetch does not support optional nested records");
        return 1;
      } else if constexpr (has_model_v<value_type>) {
        value_type sample{};
        std::size_t count = 0;
        model::for_each(sample, [&](auto member, auto, auto) {
          using field_type = std::remove_cv_t<std::remove_reference_t<decltype(sample.*member)>>;
          count += mapped_column_count<field_type>();
        });
        return count;
      } else if constexpr (model::is_tuple<value_type>::value) {
        value_type sample{};
        std::size_t count = 0;
        model::for_each(sample, [&](auto &item, auto) {
          count += mapped_column_count<decltype(item)>();
        });
        return count;
      } else {
        return 1;
      }
    }

    template <typename T>
    void assign_record(T &value, std::uint64_t row_index, std::uint64_t &column_index) const {
      using value_type = std::remove_cv_t<std::remove_reference_t<T>>;
      if constexpr (has_model_v<value_type>) {
        model::for_each(value, [&](auto member, auto, auto) {
          assign_field(value.*member, row_index, column_index);
        });
      } else {
        model::for_each(value, [&](auto &item, auto) {
          assign_field(item, row_index, column_index);
        });
      }
    }

    template <typename Field>
    void assign_field(Field &field, std::uint64_t row_index,
                      std::uint64_t &column_index) const {
      using value_type = std::remove_cv_t<std::remove_reference_t<Field>>;
      if constexpr (detail::is_optional<value_type>::value) {
        using nested_type = typename detail::is_optional<value_type>::value_type;
        static_assert(!detail::is_fetch_record<nested_type>::value,
                      "ORM typed fetch does not support optional nested records");
        if (is_null(row_index, column_index)) {
          field.reset();
          ++column_index;
          return;
        }
        nested_type nested{};
        assign_field(nested, row_index, column_index);
        field = std::move(nested);
      } else if constexpr (detail::is_fetch_record<value_type>::value) {
        assign_record(field, row_index, column_index);
      } else if constexpr (std::is_same_v<value_type, bool>) {
        field = boolean(row_index, column_index++);
      } else if constexpr (std::is_enum_v<value_type>) {
        using underlying = std::underlying_type_t<value_type>;
        underlying decoded{};
        assign_field(decoded, row_index, column_index);
        field = static_cast<value_type>(decoded);
      } else if constexpr (std::is_integral_v<value_type> && std::is_signed_v<value_type>) {
        const auto decoded = int64(row_index, column_index);
        if (decoded < static_cast<std::int64_t>(std::numeric_limits<value_type>::min()) ||
            decoded > static_cast<std::int64_t>(std::numeric_limits<value_type>::max()))
          throw status_error(ORM_STATUS_OUT_OF_RANGE,
                             "map result row: signed integer is outside the destination range");
        field = static_cast<value_type>(decoded);
        ++column_index;
      } else if constexpr (std::is_integral_v<value_type> && std::is_unsigned_v<value_type>) {
        const auto decoded = uint64(row_index, column_index);
        if (decoded > static_cast<std::uint64_t>(std::numeric_limits<value_type>::max()))
          throw status_error(ORM_STATUS_OUT_OF_RANGE,
                             "map result row: unsigned integer is outside the destination range");
        field = static_cast<value_type>(decoded);
        ++column_index;
      } else if constexpr (std::is_floating_point_v<value_type>) {
        field = static_cast<value_type>(real(row_index, column_index++));
      } else if constexpr (std::is_same_v<value_type, std::string>) {
        field = text(row_index, column_index++);
      } else if constexpr (detail::is_blob_vector<value_type>::value) {
        field = blob(row_index, column_index++);
      } else if constexpr (std::is_array_v<value_type>) {
        using element_type = std::remove_cv_t<std::remove_extent_t<value_type>>;
        static_assert(std::is_same_v<element_type, char>,
                      "ORM modeled arrays must be fixed-size character arrays");
        constexpr std::size_t capacity = std::extent_v<value_type>;
        const std::string decoded = text(row_index, column_index);
        if (decoded.size() >= capacity)
          throw status_error(ORM_STATUS_LIMIT_EXCEEDED,
                             "map result row: text does not fit in the destination character array");
        std::copy(decoded.begin(), decoded.end(), field);
        field[decoded.size()] = '\0';
        ++column_index;
      } else {
        static_assert(detail::always_false<value_type>::value,
                      "ORM modeled field type is not supported by typed fetch");
      }
    }

    orm_result_t *handle_ = nullptr;
  };

  class transaction final {
  public:
    ~transaction() noexcept { orm_transaction_destroy(handle_); }

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

    void commit() {
      checked("commit transaction", [&](orm_error_t *error) {
        return orm_transaction_commit(handle_, error);
      });
    }

    void rollback() {
      checked("roll back transaction", [&](orm_error_t *error) {
        return orm_transaction_rollback(handle_, error);
      });
    }

    transaction &savepoint(std::string_view name) {
      checked("create transaction savepoint", [&](orm_error_t *error) {
        return orm_transaction_savepoint(handle_, detail::to_view(name), error);
      });
      return *this;
    }

    transaction &rollback_to(std::string_view name) {
      checked("roll back transaction savepoint", [&](orm_error_t *error) {
        return orm_transaction_rollback_to_savepoint(
            handle_, detail::to_view(name), error);
      });
      return *this;
    }

    transaction &release(std::string_view name) {
      checked("release transaction savepoint", [&](orm_error_t *error) {
        return orm_transaction_release_savepoint(
            handle_, detail::to_view(name), error);
      });
      return *this;
    }

  private:
    friend class connection;
    friend class query;

    explicit transaction(orm_transaction_t *handle) noexcept : handle_(handle) {}

    template <typename Operation>
    void checked(std::string_view operation, Operation &&invoke) const {
      if (handle_ == nullptr) throw std::logic_error("ORM transaction has no handle");
      detail::check(operation, std::forward<Operation>(invoke));
    }

    orm_transaction_t *handle_ = nullptr;
  };

  inline result query::execute() {
    orm_result_t *output = nullptr;
    try {
      checked("execute query",
              [&](orm_error_t *error) { return orm_query_execute(handle_, &output, error); });
    } catch (...) {
      orm_result_destroy(output);
      throw;
    }
    if (output == nullptr)
      throw status_error(ORM_STATUS_INTERNAL_ERROR,
                         "execute query: success returned a null result");
    return result(output);
  }

  inline result query::execute(transaction &owner) {
    orm_result_t *output = nullptr;
    try {
      checked("execute query in transaction", [&](orm_error_t *error) {
        return orm_query_execute_in_transaction(handle_, owner.handle_, &output, error);
      });
    } catch (...) {
      orm_result_destroy(output);
      throw;
    }
    if (output == nullptr)
      throw status_error(
          ORM_STATUS_INTERNAL_ERROR,
          "execute query in transaction: success returned a null result");
    return result(output);
  }

  template <typename T> inline std::vector<T> query::fetch() {
    return execute().template as<T>();
  }

  template <typename T> inline std::vector<T> query::fetch(transaction &owner) {
    return execute(owner).template as<T>();
  }

  class select_builder;
  class select_query;
  class join_builder;
  template <typename Row> class typed_select_builder;
  template <typename Row> class typed_select_query;
  template <typename Row> class typed_join_builder;
  class insert_query;
  class update_query;
  class delete_query;
  template <typename Entity> class repository;
  template <typename Root> class polymorphic_repository;
  class session;
  template <typename Entity, typename Id> class lazy_one;
  template <typename Entity, typename ForeignKey> class lazy_many;

  namespace detail {

    template <typename T, typename = void>
    struct is_joined_entity : std::false_type {};

    template <typename T>
    struct is_joined_entity<
        T, std::void_t<decltype(model::entity_model<T>::inheritance())>>
        : std::bool_constant<model::entity_model<T>::inheritance() ==
                             model::inheritance_strategy::joined> {};

    template <typename T>
    inline constexpr bool is_joined_entity_v = is_joined_entity<T>::value;

    template <typename T, bool = is_joined_entity_v<T>>
    struct is_joined_subtype : std::false_type {};

    template <typename T>
    struct is_joined_subtype<T, true>
        : std::bool_constant<
              (model::entity_model<T>::inheritance_tables().size() > 1)> {};

    template <typename T>
    inline constexpr bool is_joined_subtype_v = is_joined_subtype<T>::value;

    template <typename T, typename = void>
    struct has_entity_graph : std::false_type {};

    template <typename T>
    struct has_entity_graph<
        T, std::void_t<decltype(model::entity_model<T>::all_graph()),
                       decltype(model::entity_model<T>::eager_graph()),
                       decltype(model::entity_model<T>::loadable_graph()),
                       decltype(model::entity_model<T>::load_graph(
                           std::declval<session &>(), std::declval<T &>(),
                           std::declval<model::entity_graph<T>>()))>>
        : std::true_type {};

    template <typename T>
    inline constexpr bool has_entity_graph_v = has_entity_graph<T>::value;

  } // namespace detail

  enum class entity_state : std::uint8_t { detached, managed, added, removed };

  namespace relation {

    enum class cascade_policy : std::uint8_t {
      none = 0,
      persist = 1,
      remove = 2,
      orphan_remove = 4,
      merge = 8,
      refresh = 16,
      detach = 32,
      all = 59
    };

    constexpr cascade_policy operator|(cascade_policy left, cascade_policy right) noexcept {
      return static_cast<cascade_policy>(static_cast<std::uint8_t>(left) |
                                         static_cast<std::uint8_t>(right));
    }

    constexpr bool includes(cascade_policy policy, cascade_policy operation) noexcept {
      return (static_cast<std::uint8_t>(policy) & static_cast<std::uint8_t>(operation)) != 0;
    }

    template <typename Owner, typename Child, typename... Nested> struct has_one final {
      Child Owner::*member;
      cascade_policy policy;
      std::function<void(const Owner &, Child &)> bind_foreign_key;
      std::tuple<Nested...> nested;
    };

    template <typename Owner, typename Child, typename... Nested> struct has_many final {
      std::vector<Child> Owner::*member;
      cascade_policy policy;
      std::function<void(const Owner &, Child &)> bind_foreign_key;
      std::tuple<Nested...> nested;
    };

    template <typename Owner, typename Child, typename... Nested> struct optional_one final {
      std::optional<Child> Owner::*member;
      cascade_policy policy;
      std::function<void(const Owner &, Child &)> bind_foreign_key;
      std::tuple<Nested...> nested;
    };

    template <typename Owner, typename Child, typename... Nested> struct shared_one final {
      std::shared_ptr<Child> Owner::*member;
      cascade_policy policy;
      std::function<void(const Owner &, Child &)> bind_foreign_key;
      std::tuple<Nested...> nested;
    };

    template <typename Owner, typename Child, typename... Nested> struct shared_many final {
      std::vector<std::shared_ptr<Child>> Owner::*member;
      cascade_policy policy;
      std::function<void(const Owner &, Child &)> bind_foreign_key;
      std::tuple<Nested...> nested;
    };

    template <typename Owner, typename Child, typename OwnerKey, typename ChildKey>
    std::function<void(const Owner &, Child &)> foreign_key_binder(OwnerKey Owner::*owner_key,
                                                                    ChildKey Child::*child_key) {
      static_assert(std::is_assignable_v<ChildKey &, const OwnerKey &>,
                    "ORM relation foreign key fields must be assignable");
      return [owner_key, child_key](const Owner &owner, Child &child) {
        child.*child_key = owner.*owner_key;
      };
    }

    template <typename Owner, typename Child, typename... Nested>
    has_one<Owner, Child, std::decay_t<Nested>...> one(Child Owner::*member,
                                                        cascade_policy policy,
                                                        Nested &&...nested) {
      return {member, policy, {},
              std::tuple<std::decay_t<Nested>...>(std::forward<Nested>(nested)...)};
    }

    template <typename Owner, typename Child, typename OwnerKey, typename ChildKey,
              typename... Nested>
    has_one<Owner, Child, std::decay_t<Nested>...> one(
        Child Owner::*member, OwnerKey Owner::*owner_key, ChildKey Child::*child_key,
        cascade_policy policy, Nested &&...nested) {
      return {member, policy, foreign_key_binder(owner_key, child_key),
              std::tuple<std::decay_t<Nested>...>(std::forward<Nested>(nested)...)};
    }

    template <typename Owner, typename Child, typename... Nested>
    has_many<Owner, Child, std::decay_t<Nested>...> many(std::vector<Child> Owner::*member,
                                                         cascade_policy policy,
                                                         Nested &&...nested) {
      return {member, policy, {},
              std::tuple<std::decay_t<Nested>...>(std::forward<Nested>(nested)...)};
    }

    template <typename Owner, typename Child, typename OwnerKey, typename ChildKey,
              typename... Nested>
    has_many<Owner, Child, std::decay_t<Nested>...> many(
        std::vector<Child> Owner::*member, OwnerKey Owner::*owner_key, ChildKey Child::*child_key,
        cascade_policy policy, Nested &&...nested) {
      return {member, policy, foreign_key_binder(owner_key, child_key),
              std::tuple<std::decay_t<Nested>...>(std::forward<Nested>(nested)...)};
    }

    template <typename Owner, typename Child, typename... Nested>
    optional_one<Owner, Child, std::decay_t<Nested>...> one(
        std::optional<Child> Owner::*member, cascade_policy policy, Nested &&...nested) {
      return {member, policy, {},
              std::tuple<std::decay_t<Nested>...>(std::forward<Nested>(nested)...)};
    }

    template <typename Owner, typename Child, typename OwnerKey, typename ChildKey,
              typename... Nested>
    optional_one<Owner, Child, std::decay_t<Nested>...> one(
        std::optional<Child> Owner::*member, OwnerKey Owner::*owner_key,
        ChildKey Child::*child_key, cascade_policy policy, Nested &&...nested) {
      return {member, policy, foreign_key_binder(owner_key, child_key),
              std::tuple<std::decay_t<Nested>...>(std::forward<Nested>(nested)...)};
    }

    template <typename Owner, typename Child, typename... Nested>
    shared_one<Owner, Child, std::decay_t<Nested>...> one(
        std::shared_ptr<Child> Owner::*member, cascade_policy policy, Nested &&...nested) {
      return {member, policy, {},
              std::tuple<std::decay_t<Nested>...>(std::forward<Nested>(nested)...)};
    }

    template <typename Owner, typename Child, typename OwnerKey, typename ChildKey,
              typename... Nested>
    shared_one<Owner, Child, std::decay_t<Nested>...> one(
        std::shared_ptr<Child> Owner::*member, OwnerKey Owner::*owner_key,
        ChildKey Child::*child_key, cascade_policy policy, Nested &&...nested) {
      return {member, policy, foreign_key_binder(owner_key, child_key),
              std::tuple<std::decay_t<Nested>...>(std::forward<Nested>(nested)...)};
    }

    template <typename Owner, typename Child, typename... Nested>
    shared_many<Owner, Child, std::decay_t<Nested>...> many(
        std::vector<std::shared_ptr<Child>> Owner::*member, cascade_policy policy,
        Nested &&...nested) {
      return {member, policy, {},
              std::tuple<std::decay_t<Nested>...>(std::forward<Nested>(nested)...)};
    }

    template <typename Owner, typename Child, typename OwnerKey, typename ChildKey,
              typename... Nested>
    shared_many<Owner, Child, std::decay_t<Nested>...> many(
        std::vector<std::shared_ptr<Child>> Owner::*member, OwnerKey Owner::*owner_key,
        ChildKey Child::*child_key, cascade_policy policy, Nested &&...nested) {
      return {member, policy, foreign_key_binder(owner_key, child_key),
              std::tuple<std::decay_t<Nested>...>(std::forward<Nested>(nested)...)};
    }

  } // namespace relation

  class connection final {
  public:
    explicit connection(const config &configuration) {
      if (configuration.options_.size() >
          static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        throw std::length_error("ORM option count exceeds the C ABI range");

      std::vector<orm_option_t> options;
      options.reserve(configuration.options_.size());
      for (const auto &option : configuration.options_) {
        options.push_back({detail::to_view(option.first), detail::to_view(option.second)});
      }

      orm_config_t native = configuration.native_;
      native.driver = detail::to_view(configuration.driver_);
      native.options = options.empty() ? nullptr : options.data();
      native.option_count = static_cast<std::uint32_t>(options.size());
      orm_connection_t *output = nullptr;
      try {
        detail::check("connect",
                      [&](orm_error_t *error) { return orm_connect(&native, &output, error); });
      } catch (...) {
        orm_disconnect(output);
        throw;
      }
      if (output == nullptr)
        throw status_error(ORM_STATUS_INTERNAL_ERROR,
                           "connect: success returned a null connection");
      handle_ = output;
    }

    ~connection() noexcept { orm_disconnect(handle_); }

    connection(const connection &) = delete;
    connection &operator=(const connection &) = delete;

    connection(connection &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    connection &operator=(connection &&other) noexcept {
      if (this != &other) {
        orm_disconnect(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
      }
      return *this;
    }

    [[nodiscard]] query select(std::string_view table) const {
      return create_query("create SELECT query", orm_query_create, table);
    }

    [[nodiscard]] query insert(std::string_view table) const {
      return create_query("create INSERT query", orm_insert, table);
    }

    [[nodiscard]] query update(std::string_view table) const {
      return create_query("create UPDATE query", orm_update, table);
    }

    [[nodiscard]] query delete_from(std::string_view table) const {
      return create_query("create DELETE query", orm_delete, table);
    }

    [[nodiscard]] query raw(std::string_view sql) const {
      return create_query("create raw query", orm_raw, sql);
    }

    [[nodiscard]] transaction begin_transaction(
        isolation_level isolation = isolation_level::serializable) const {
      if (handle_ == nullptr) throw std::logic_error("ORM connection has no handle");
      orm_transaction_t *output = nullptr;
      try {
        detail::check("begin transaction", [&](orm_error_t *error) {
          return orm_transaction_begin(handle_, static_cast<orm_isolation_t>(isolation),
                                       &output, error);
        });
      } catch (...) {
        orm_transaction_destroy(output);
        throw;
      }
      if (output == nullptr)
        throw status_error(ORM_STATUS_INTERNAL_ERROR,
                           "begin transaction: success returned a null transaction");
      return transaction(output);
    }

    template <typename... Projections,
              std::enable_if_t<(detail::is_projection_v<Projections> && ...), int> = 0>
    [[nodiscard]] typed_select_builder<detail::projection_row_t<Projections...>>
    select(Projections &&...projections) const;

    template <typename Entity,
              std::enable_if_t<detail::has_model_v<Entity>, int> = 0>
    [[nodiscard]] select_query select() const;

    template <typename Entity> [[nodiscard]] insert_query insert(const table<Entity> &target) const;

    template <typename Entity> [[nodiscard]] update_query update(const table<Entity> &target) const;

    template <typename Entity>
    [[nodiscard]] delete_query delete_from(const table<Entity> &target) const;

  private:
    using query_factory = orm_status_t(ORM_C_CALL *)(orm_connection_t *, orm_string_view_t,
                                                     orm_query_t **, orm_error_t *);

    [[nodiscard]] query create_query(std::string_view operation, query_factory factory,
                                     std::string_view input) const {
      if (handle_ == nullptr) throw std::logic_error("ORM connection has no handle");
      orm_query_t *output = nullptr;
      try {
        detail::check(operation, [&](orm_error_t *error) {
          return factory(handle_, detail::to_view(input), &output, error);
        });
      } catch (...) {
        orm_query_destroy(output);
        throw;
      }
      if (output == nullptr)
        throw status_error(ORM_STATUS_INTERNAL_ERROR,
                           std::string(operation) + ": success returned a null query");
      return query(output);
    }

    orm_connection_t *handle_ = nullptr;
  };

  class join_builder final {
  public:
    [[nodiscard]] select_query &on(const join_condition &condition);

  private:
    friend class select_query;

    join_builder(select_query &owner, join_type type, std::string table_name)
        : owner_(&owner), type_(type), table_(std::move(table_name)) {}

    select_query *owner_;
    join_type type_;
    std::string table_;
  };

  class select_query final {
  public:
    select_query(const select_query &) = delete;
    select_query &operator=(const select_query &) = delete;
    select_query(select_query &&) noexcept = default;
    select_query &operator=(select_query &&) noexcept = default;

    select_query &where(const predicate &condition) {
      detail::apply_where(query_, condition);
      return *this;
    }

    select_query &where(const exists_predicate &condition) {
      condition.apply(query_);
      return *this;
    }

    template <typename T>
    select_query &where_in(const column<T> &column, const select_query &subquery) {
      query_.where_in_subquery(column.qualified_name(), subquery.query_);
      return *this;
    }

    template <typename T>
    select_query &where_not_in(const column<T> &column, const select_query &subquery) {
      query_.where_in_subquery(column.qualified_name(), subquery.query_, true);
      return *this;
    }

    template <typename T, typename Row,
              typename Projection = detail::subquery_projection<Row>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    select_query &where_in(const column<T> &column,
                           const typed_select_query<Row> &subquery) {
      subquery.apply_in_subquery(query_, column, false);
      return *this;
    }

    template <typename T, typename Row,
              typename Projection = detail::subquery_projection<Row>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    select_query &where_not_in(const column<T> &column,
                               const typed_select_query<Row> &subquery) {
      subquery.apply_in_subquery(query_, column, true);
      return *this;
    }

    template <typename T, typename Row,
              typename Projection = detail::subquery_projection<Row>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    select_query &where(const column<T> &column, comparison operation,
                        const typed_select_query<Row> &subquery) {
      subquery.apply_scalar_subquery(query_, column, operation);
      return *this;
    }

    template <typename T, typename Row,
              typename Projection = detail::subquery_projection<Row>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    select_query &where_any(const column<T> &column, comparison operation,
                            const typed_select_query<Row> &subquery) {
      subquery.apply_quantified_subquery(query_, column, operation,
                                         subquery_quantifier::any);
      return *this;
    }

    template <typename T, typename Row,
              typename Projection = detail::subquery_projection<Row>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    select_query &where_all(const column<T> &column, comparison operation,
                            const typed_select_query<Row> &subquery) {
      subquery.apply_quantified_subquery(query_, column, operation,
                                         subquery_quantifier::all);
      return *this;
    }

    void apply_in_subquery(query &outer, std::string_view column_name,
                           bool negated) const {
      outer.where_in_subquery(column_name, query_, negated);
    }

    void apply_scalar_subquery(query &outer, std::string_view column_name,
                               comparison operation) const {
      outer.where_scalar_subquery(column_name, operation, query_);
    }

    void apply_quantified_subquery(query &outer, std::string_view column_name,
                                   comparison operation,
                                   subquery_quantifier quantifier) const {
      outer.where_quantified_subquery(column_name, operation, quantifier, query_);
    }

    [[nodiscard]] exists_predicate exists() {
      return exists_predicate(std::move(query_), false);
    }

    [[nodiscard]] exists_predicate not_exists() {
      return exists_predicate(std::move(query_), true);
    }

    select_query &distinct(bool enabled = true) {
      query_.distinct(enabled);
      return *this;
    }

    select_query &where(std::string_view column_name, comparison operation, value input) {
      query_.where(column_name, operation, std::move(input));
      return *this;
    }

    template <typename Entity> [[nodiscard]] join_builder inner_join(const table<Entity> &target) {
      return join_builder(*this, join_type::inner, std::string(target.name()));
    }

    template <typename Entity> [[nodiscard]] join_builder left_join(const table<Entity> &target) {
      return join_builder(*this, join_type::left, std::string(target.name()));
    }

    template <typename T> select_query &group_by(const column<T> &input) {
      query_.group_by(input.qualified_name());
      return *this;
    }

    select_query &having(const predicate &condition) {
      detail::apply_having(query_, condition);
      return *this;
    }

    select_query &order_by(const order_specifier &order) {
      query_.order_by(order.column_name(), order.order());
      return *this;
    }

    template <typename T> select_query &order_by(const expression<T> &input,
                                                sort_order order) {
      query_.order_by(input, order);
      return *this;
    }

    select_query &limit(std::uint64_t count) {
      query_.limit(count);
      return *this;
    }

    select_query &offset(std::uint64_t count) {
      query_.offset(count);
      return *this;
    }

    [[nodiscard]] result fetch() { return query_.execute(); }
    [[nodiscard]] result fetch(transaction &owner) { return query_.execute(owner); }

    template <typename T> [[nodiscard]] std::vector<T> fetch() {
      return query_.template fetch<T>();
    }

    template <typename T> [[nodiscard]] std::vector<T> fetch(transaction &owner) {
      return query_.template fetch<T>(owner);
    }

  private:
    friend class connection;
    friend class select_builder;
    friend class join_builder;

    explicit select_query(query target) : query_(std::move(target)) {}

    query query_;
  };

  template <typename Entity, std::enable_if_t<detail::has_model_v<Entity>, int>>
  select_query connection::select() const {
    using value_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
    query target = select(model::get_name<value_type>());
    if constexpr (detail::is_joined_entity_v<value_type>) {
      constexpr auto tables =
          model::entity_model<value_type>::inheritance_tables();
      constexpr auto column_tables =
          model::entity_model<value_type>::column_tables();
      constexpr auto keys = model::get_primary_keys<value_type>();
      static_assert(keys.size() == 1,
                    "ORM joined inheritance requires one primary-key column");
      std::size_t column_index = 0;
      detail::for_each_flat_field_name<value_type>(
          [&](std::string_view field_name) {
            if (column_index >= column_tables.size())
              throw std::logic_error(
                  "ORM joined column-table metadata is incomplete");
            target.column(std::string(column_tables[column_index]) + "." +
                          std::string(field_name));
            ++column_index;
          });
      if (column_index != column_tables.size())
        throw std::logic_error(
            "ORM joined column-table metadata does not match modeled fields");
      for (std::size_t index = 1; index < tables.size(); ++index) {
        target.join(join_type::inner, tables[index],
                    std::string(tables.front()) + "." +
                        std::string(keys.front()),
                    comparison::equal,
                    std::string(tables[index]) + "." +
                        std::string(keys.front()));
      }
    } else {
      detail::for_each_flat_field_name<value_type>(
          [&](std::string_view field_name) { target.column(field_name); });
    }
    if constexpr (model::has_discriminator_metadata_v<value_type>) {
      std::string discriminator_column(
          model::get_discriminator_column<value_type>());
      if constexpr (detail::is_joined_entity_v<value_type>) {
        discriminator_column =
            std::string(model::entity_model<value_type>::inheritance_tables()
                            .front()) +
            "." + discriminator_column;
      }
      target.where(discriminator_column,
                   comparison::equal,
                   detail::model_value(
                       model::get_discriminator_value<value_type>()));
    }
    return select_query(std::move(target));
  }

  inline select_query &join_builder::on(const join_condition &condition) {
    const bool left_is_joined = condition.left_table_ == table_;
    const bool right_is_joined = condition.right_table_ == table_;
    if (left_is_joined == right_is_joined)
      throw std::invalid_argument(
          "ORM JOIN condition must reference the joined table on exactly one side");
    owner_->query_.join(type_, table_, condition.left_column_, condition.operation_,
                        condition.right_column_);
    return *owner_;
  }

  class select_builder final {
  public:
    template <typename Entity> [[nodiscard]] select_query from(const table<Entity> &source) const {
      query target = connection_->select(source.name());
      if (projections_.empty()) {
        target.select_all();
      } else {
        for (const auto &projection : projections_) {
          if (projection.type == detail::projection_spec::kind::column) {
            target.column(projection.column);
          } else if (projection.type == detail::projection_spec::kind::aggregate) {
            target.aggregate(projection.function, projection.column, projection.alias);
          } else {
            target.scalar_expression(projection.tokens, projection.alias);
          }
        }
      }
      return select_query(std::move(target));
    }

  private:
    friend class connection;
    template <typename> friend class typed_select_builder;

    select_builder(const connection &owner, std::vector<detail::projection_spec> projections)
        : connection_(&owner), projections_(std::move(projections)) {}

    const connection *connection_;
    std::vector<detail::projection_spec> projections_;
  };

  template <typename Row> class typed_join_builder final {
  public:
    [[nodiscard]] typed_select_query<Row> &on(const join_condition &condition) & {
      (void)builder_.on(condition);
      owner_->allow_table(table_);
      return *owner_;
    }

    [[nodiscard]] typed_select_query<Row> &&on(const join_condition &condition) && {
      (void)builder_.on(condition);
      owner_->allow_table(table_);
      return std::move(*owner_);
    }

  private:
    friend class typed_select_query<Row>;

    typed_join_builder(typed_select_query<Row> &owner, join_builder builder,
                       std::string table_name)
        : owner_(&owner), builder_(std::move(builder)), table_(std::move(table_name)) {}

    typed_select_query<Row> *owner_;
    join_builder builder_;
    std::string table_;
  };

  template <typename Row> class typed_select_query final {
  public:
    using row_type = Row;

    typed_select_query(const typed_select_query &) = delete;
    typed_select_query &operator=(const typed_select_query &) = delete;
    typed_select_query(typed_select_query &&) noexcept = default;
    typed_select_query &operator=(typed_select_query &&) noexcept = default;

    typed_select_query &where(const predicate &condition) & {
      query_.where(condition);
      return *this;
    }

    typed_select_query &&where(const predicate &condition) && {
      query_.where(condition);
      return std::move(*this);
    }

    typed_select_query &where(const exists_predicate &condition) & {
      query_.where(condition);
      return *this;
    }

    typed_select_query &&where(const exists_predicate &condition) && {
      query_.where(condition);
      return std::move(*this);
    }

    template <typename T, typename Projection = detail::subquery_projection<Row>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    void apply_in_subquery(query &outer, const column<T> &column, bool negated) const {
      query_.apply_in_subquery(outer, column.qualified_name(), negated);
    }

    template <typename T, typename Projection = detail::subquery_projection<Row>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    void apply_scalar_subquery(query &outer, const column<T> &column,
                               comparison operation) const {
      query_.apply_scalar_subquery(outer, column.qualified_name(), operation);
    }

    template <typename T, typename Projection = detail::subquery_projection<Row>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    void apply_quantified_subquery(query &outer, const column<T> &column,
                                   comparison operation,
                                   subquery_quantifier quantifier) const {
      query_.apply_quantified_subquery(outer, column.qualified_name(), operation,
                                       quantifier);
    }

    template <typename T, typename InnerRow,
              typename Projection = detail::subquery_projection<InnerRow>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    typed_select_query &where(const column<T> &column, comparison operation,
                              const typed_select_query<InnerRow> &subquery) & {
      query_.where(column, operation, subquery);
      return *this;
    }

    template <typename T, typename InnerRow,
              typename Projection = detail::subquery_projection<InnerRow>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    typed_select_query &&where(const column<T> &column, comparison operation,
                               const typed_select_query<InnerRow> &subquery) && {
      query_.where(column, operation, subquery);
      return std::move(*this);
    }

    template <typename T, typename InnerRow,
              typename Projection = detail::subquery_projection<InnerRow>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    typed_select_query &where_any(const column<T> &column, comparison operation,
                                  const typed_select_query<InnerRow> &subquery) & {
      query_.where_any(column, operation, subquery);
      return *this;
    }

    template <typename T, typename InnerRow,
              typename Projection = detail::subquery_projection<InnerRow>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    typed_select_query &&where_any(const column<T> &column, comparison operation,
                                   const typed_select_query<InnerRow> &subquery) && {
      query_.where_any(column, operation, subquery);
      return std::move(*this);
    }

    template <typename T, typename InnerRow,
              typename Projection = detail::subquery_projection<InnerRow>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    typed_select_query &where_all(const column<T> &column, comparison operation,
                                  const typed_select_query<InnerRow> &subquery) & {
      query_.where_all(column, operation, subquery);
      return *this;
    }

    template <typename T, typename InnerRow,
              typename Projection = detail::subquery_projection<InnerRow>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    typed_select_query &&where_all(const column<T> &column, comparison operation,
                                   const typed_select_query<InnerRow> &subquery) && {
      query_.where_all(column, operation, subquery);
      return std::move(*this);
    }

    template <typename T, typename InnerRow,
              typename Projection = detail::subquery_projection<InnerRow>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    typed_select_query &where_in(const column<T> &column,
                                 const typed_select_query<InnerRow> &subquery) & {
      query_.where_in(column, subquery);
      return *this;
    }

    template <typename T, typename InnerRow,
              typename Projection = detail::subquery_projection<InnerRow>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    typed_select_query &&where_in(const column<T> &column,
                                  const typed_select_query<InnerRow> &subquery) && {
      query_.where_in(column, subquery);
      return std::move(*this);
    }

    template <typename T, typename InnerRow,
              typename Projection = detail::subquery_projection<InnerRow>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    typed_select_query &where_not_in(const column<T> &column,
                                     const typed_select_query<InnerRow> &subquery) & {
      query_.where_not_in(column, subquery);
      return *this;
    }

    template <typename T, typename InnerRow,
              typename Projection = detail::subquery_projection<InnerRow>,
              std::enable_if_t<Projection::valid &&
                                   detail::is_compatible_column_v<T, typename Projection::type>,
                               int> = 0>
    typed_select_query &&where_not_in(const column<T> &column,
                                      const typed_select_query<InnerRow> &subquery) && {
      query_.where_not_in(column, subquery);
      return std::move(*this);
    }

    [[nodiscard]] exists_predicate exists() {
      return query_.exists();
    }

    [[nodiscard]] exists_predicate not_exists() {
      return query_.not_exists();
    }

    typed_select_query &distinct(bool enabled = true) & {
      query_.distinct(enabled);
      return *this;
    }

    typed_select_query &&distinct(bool enabled = true) && {
      query_.distinct(enabled);
      return std::move(*this);
    }

    typed_select_query &where(std::string_view column_name, comparison operation,
                              value input) & {
      query_.where(column_name, operation, std::move(input));
      return *this;
    }

    typed_select_query &&where(std::string_view column_name, comparison operation,
                               value input) && {
      query_.where(column_name, operation, std::move(input));
      return std::move(*this);
    }

    template <typename Entity>
    [[nodiscard]] typed_join_builder<Row> inner_join(const table<Entity> &target) {
      return typed_join_builder<Row>(*this, query_.inner_join(target), std::string(target.name()));
    }

    template <typename Entity>
    [[nodiscard]] typed_join_builder<Row> left_join(const table<Entity> &target) {
      return typed_join_builder<Row>(*this, query_.left_join(target), std::string(target.name()));
    }

    template <typename T> typed_select_query &group_by(const column<T> &input) & {
      query_.group_by(input);
      return *this;
    }

    template <typename T> typed_select_query &&group_by(const column<T> &input) && {
      query_.group_by(input);
      return std::move(*this);
    }

    typed_select_query &having(const predicate &condition) & {
      query_.having(condition);
      return *this;
    }

    typed_select_query &&having(const predicate &condition) && {
      query_.having(condition);
      return std::move(*this);
    }

    typed_select_query &order_by(const order_specifier &order) & {
      query_.order_by(order);
      return *this;
    }

    typed_select_query &&order_by(const order_specifier &order) && {
      query_.order_by(order);
      return std::move(*this);
    }

    template <typename T> typed_select_query &order_by(const expression<T> &input,
                                                      sort_order order) & {
      query_.order_by(input, order);
      return *this;
    }

    template <typename T> typed_select_query &&order_by(const expression<T> &input,
                                                       sort_order order) && {
      query_.order_by(input, order);
      return std::move(*this);
    }

    typed_select_query &limit(std::uint64_t count) & {
      query_.limit(count);
      return *this;
    }

    typed_select_query &&limit(std::uint64_t count) && {
      query_.limit(count);
      return std::move(*this);
    }

    typed_select_query &offset(std::uint64_t count) & {
      query_.offset(count);
      return *this;
    }

    typed_select_query &&offset(std::uint64_t count) && {
      query_.offset(count);
      return std::move(*this);
    }

    [[nodiscard]] result fetch() {
      validate_projection_tables();
      return query_.fetch();
    }

    [[nodiscard]] result fetch(transaction &owner) {
      validate_projection_tables();
      return query_.fetch(owner);
    }

    [[nodiscard]] std::vector<Row> fetch_typed() {
      require_explicit_projection();
      validate_projection_tables();
      return query_.template fetch<Row>();
    }

    [[nodiscard]] std::vector<Row> fetch_typed(transaction &owner) {
      require_explicit_projection();
      validate_projection_tables();
      return query_.template fetch<Row>(owner);
    }

    [[nodiscard]] std::optional<Row> fetch_one_typed() {
      require_explicit_projection();
      validate_projection_tables();
      return materialize_one(query_.fetch());
    }

    [[nodiscard]] std::optional<Row> fetch_one_typed(transaction &owner) {
      require_explicit_projection();
      validate_projection_tables();
      return materialize_one(query_.fetch(owner));
    }

    template <typename R = Row,
              typename Scalar = typename detail::single_projection_value<R>::type>
    [[nodiscard]] std::vector<Scalar> fetch_scalars() {
      auto rows = fetch_typed();
      std::vector<Scalar> output;
      output.reserve(rows.size());
      for (auto &row : rows) output.push_back(std::move(std::get<0>(row)));
      return output;
    }

    template <typename R = Row,
              typename Scalar = typename detail::single_projection_value<R>::type>
    [[nodiscard]] std::vector<Scalar> fetch_scalars(transaction &owner) {
      auto rows = fetch_typed(owner);
      std::vector<Scalar> output;
      output.reserve(rows.size());
      for (auto &row : rows) output.push_back(std::move(std::get<0>(row)));
      return output;
    }

    template <typename R = Row,
              typename Scalar = typename detail::single_projection_value<R>::type>
    [[nodiscard]] std::optional<Scalar> fetch_one_scalar() {
      auto row = fetch_one_typed();
      if (!row.has_value()) return std::nullopt;
      return std::move(std::get<0>(*row));
    }

    template <typename R = Row,
              typename Scalar = typename detail::single_projection_value<R>::type>
    [[nodiscard]] std::optional<Scalar> fetch_one_scalar(transaction &owner) {
      auto row = fetch_one_typed(owner);
      if (!row.has_value()) return std::nullopt;
      return std::move(std::get<0>(*row));
    }

    template <typename Mapper, typename Traits = detail::projection_mapper<Row, Mapper>,
              std::enable_if_t<Traits::valid, int> = 0>
    [[nodiscard]] std::vector<typename Traits::result_type> fetch_mapped(Mapper &&mapper) {
      auto rows = fetch_typed();
      std::vector<typename Traits::result_type> output;
      output.reserve(rows.size());
      for (auto &row : rows)
        output.push_back(std::apply(mapper, std::move(row)));
      return output;
    }

    template <typename Mapper, typename Traits = detail::projection_mapper<Row, Mapper>,
              std::enable_if_t<Traits::valid, int> = 0>
    [[nodiscard]] std::vector<typename Traits::result_type>
    fetch_mapped(Mapper &&mapper, transaction &owner) {
      auto rows = fetch_typed(owner);
      std::vector<typename Traits::result_type> output;
      output.reserve(rows.size());
      for (auto &row : rows)
        output.push_back(std::apply(mapper, std::move(row)));
      return output;
    }

    template <typename Mapper, typename Traits = detail::projection_mapper<Row, Mapper>,
              std::enable_if_t<Traits::valid, int> = 0>
    [[nodiscard]] std::optional<typename Traits::result_type>
    fetch_one_mapped(Mapper &&mapper) {
      auto row = fetch_one_typed();
      if (!row.has_value()) return std::nullopt;
      return std::apply(mapper, std::move(*row));
    }

    template <typename Mapper, typename Traits = detail::projection_mapper<Row, Mapper>,
              std::enable_if_t<Traits::valid, int> = 0>
    [[nodiscard]] std::optional<typename Traits::result_type>
    fetch_one_mapped(Mapper &&mapper, transaction &owner) {
      auto row = fetch_one_typed(owner);
      if (!row.has_value()) return std::nullopt;
      return std::apply(mapper, std::move(*row));
    }

    template <typename T> [[nodiscard]] std::vector<T> fetch() {
      validate_projection_tables();
      return query_.template fetch<T>();
    }

    template <typename T> [[nodiscard]] std::vector<T> fetch(transaction &owner) {
      validate_projection_tables();
      return query_.template fetch<T>(owner);
    }

  private:
    friend class typed_select_builder<Row>;
    friend class typed_join_builder<Row>;

    typed_select_query(select_query target, std::string source_table,
                       std::vector<std::string> projection_columns)
        : query_(std::move(target)), projection_columns_(std::move(projection_columns)) {
      allowed_tables_.insert(std::move(source_table));
    }

    void allow_table(const std::string &table_name) { allowed_tables_.insert(table_name); }

    void validate_projection_tables() const {
      for (const auto &column_name : projection_columns_) {
        const bool allowed = std::any_of(
            allowed_tables_.begin(), allowed_tables_.end(), [&](const auto &table_name) {
              return column_name.size() > table_name.size() &&
                     column_name.compare(0, table_name.size(), table_name) == 0 &&
                     column_name[table_name.size()] == '.';
            });
        if (!allowed)
          throw std::invalid_argument(
              "ORM projection column belongs to neither the FROM table nor a joined table");
      }
    }

    static constexpr void require_explicit_projection() {
      static_assert(std::tuple_size<Row>::value != 0,
                    "ORM typed fetch requires at least one explicit projection");
    }

    static std::optional<Row> materialize_one(result output) {
      const std::uint64_t count = output.rows();
      if (count == 0) return std::nullopt;
      if (count != 1)
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "typed single-result query returned more than one row");
      return output.template row<Row>(0);
    }

    select_query query_;
    std::vector<std::string> projection_columns_;
    std::unordered_set<std::string> allowed_tables_;
  };

  template <typename Row> class typed_select_builder final {
  public:
    using row_type = Row;

    template <typename Entity>
    [[nodiscard]] typed_select_query<Row> from(const table<Entity> &source) const {
      std::vector<std::string> projection_columns;
      projection_columns.reserve(builder_.projections_.size());
      for (const auto &projection : builder_.projections_)
        if (!projection.column.empty()) projection_columns.push_back(projection.column);
      return typed_select_query<Row>(builder_.from(source), std::string(source.name()),
                                     std::move(projection_columns));
    }

  private:
    friend class connection;

    explicit typed_select_builder(select_builder builder) : builder_(std::move(builder)) {}

    select_builder builder_;
  };

  class insert_query final {
  public:
    insert_query(const insert_query &) = delete;
    insert_query &operator=(const insert_query &) = delete;
    insert_query(insert_query &&) noexcept = default;
    insert_query &operator=(insert_query &&) noexcept = default;

    template <typename T, typename Input,
              std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    insert_query &set(const column<T> &target, Input &&input) {
      check_assignment_table(target.table_name());
      query_.set(target.name(), value(std::forward<Input>(input)));
      return *this;
    }

    [[nodiscard]] result execute() { return query_.execute(); }
    [[nodiscard]] result execute(transaction &owner) { return query_.execute(owner); }

  private:
    friend class connection;

    insert_query(query target, std::string table_name)
        : query_(std::move(target)), table_(std::move(table_name)) {}

    void check_assignment_table(std::string_view input) const {
      if (input != table_)
        throw std::invalid_argument("ORM assignment column belongs to a different table");
    }

    query query_;
    std::string table_;
  };

  class update_query final {
  public:
    update_query(const update_query &) = delete;
    update_query &operator=(const update_query &) = delete;
    update_query(update_query &&) noexcept = default;
    update_query &operator=(update_query &&) noexcept = default;

    template <typename T, typename Input,
              std::enable_if_t<detail::is_compatible_value_v<T, Input>, int> = 0>
    update_query &set(const column<T> &target, Input &&input) {
      if (target.table_name() != table_)
        throw std::invalid_argument("ORM assignment column belongs to a different table");
      query_.set(target.name(), value(std::forward<Input>(input)));
      return *this;
    }

    update_query &where(const predicate &condition) {
      detail::apply_where(query_, condition, table_);
      return *this;
    }

    [[nodiscard]] result execute() { return query_.execute(); }
    [[nodiscard]] result execute(transaction &owner) { return query_.execute(owner); }

  private:
    friend class connection;

    update_query(query target, std::string table_name)
        : query_(std::move(target)), table_(std::move(table_name)) {}

    query query_;
    std::string table_;
  };

  class delete_query final {
  public:
    delete_query(const delete_query &) = delete;
    delete_query &operator=(const delete_query &) = delete;
    delete_query(delete_query &&) noexcept = default;
    delete_query &operator=(delete_query &&) noexcept = default;

    delete_query &where(const predicate &condition) {
      detail::apply_where(query_, condition, table_);
      return *this;
    }

    [[nodiscard]] result execute() { return query_.execute(); }
    [[nodiscard]] result execute(transaction &owner) { return query_.execute(owner); }

  private:
    friend class connection;

    delete_query(query target, std::string table_name)
        : query_(std::move(target)), table_(std::move(table_name)) {}

    query query_;
    std::string table_;
  };

  template <typename... Projections,
            std::enable_if_t<(detail::is_projection_v<Projections> && ...), int>>
  typed_select_builder<detail::projection_row_t<Projections...>>
  connection::select(Projections &&...projections) const {
    std::vector<detail::projection_spec> specifications;
    specifications.reserve(sizeof...(Projections));
    (specifications.push_back(detail::projection_of(std::forward<Projections>(projections))), ...);
    return typed_select_builder<detail::projection_row_t<Projections...>>(
        select_builder(*this, std::move(specifications)));
  }

  template <typename Entity> insert_query connection::insert(const table<Entity> &target) const {
    return insert_query(insert(target.name()), std::string(target.name()));
  }

  template <typename Entity> update_query connection::update(const table<Entity> &target) const {
    return update_query(update(target.name()), std::string(target.name()));
  }

  template <typename Entity>
  delete_query connection::delete_from(const table<Entity> &target) const {
    return delete_query(delete_from(target.name()), std::string(target.name()));
  }

  namespace detail {
    template <typename Entity> struct managed_entry;
  }

  template <typename Entity> class repository final {
  public:
    using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;

    static_assert(detail::has_model_v<entity_type>,
                  "ORM repository<T> requires an entity model");
    static_assert(!model::get_name<entity_type>().empty() &&
                      model::get_primary_keys<entity_type>().size() != 0,
                  "ORM repository<T> requires a persistent entity model");
    static_assert(std::is_default_constructible_v<entity_type>,
                  "ORM repository<T> requires a default-constructible entity type");

    explicit repository(const connection &owner) noexcept : connection_(&owner) {}

    [[nodiscard]] std::vector<entity_type> find_all() const {
      return connection_->template select<entity_type>().template fetch<entity_type>();
    }

    [[nodiscard]] std::vector<entity_type> find_all(transaction &owner) const {
      return connection_->template select<entity_type>().template fetch<entity_type>(owner);
    }

    template <typename Id> [[nodiscard]] std::optional<entity_type> find_by_id(Id &&id) const {
      auto target = select_by_id(std::forward<Id>(id));
      auto rows = target.template fetch<entity_type>();
      if (rows.empty()) return std::nullopt;
      return std::move(rows.front());
    }

    template <typename Id>
    [[nodiscard]] std::optional<entity_type> find_by_id(Id &&id, transaction &owner) const {
      auto target = select_by_id(std::forward<Id>(id));
      auto rows = target.template fetch<entity_type>(owner);
      if (rows.empty()) return std::nullopt;
      return std::move(rows.front());
    }

    [[nodiscard]] result insert(const entity_type &entity) const {
      if constexpr (detail::is_joined_subtype_v<entity_type>) {
        return execute_owned(
            [&](transaction &owner) { return joined_insert(entity, owner); });
      } else {
        return make_insert(entity).execute();
      }
    }

    [[nodiscard]] result insert(const entity_type &entity, transaction &owner) const {
      if constexpr (detail::is_joined_subtype_v<entity_type>) {
        return joined_insert(entity, owner);
      } else {
        return make_insert(entity).execute(owner);
      }
    }

    [[nodiscard]] result update(entity_type &entity) const {
      return execute_update(entity, nullptr);
    }

    [[nodiscard]] result update(entity_type &entity, transaction &owner) const {
      return execute_update(entity, &owner);
    }

    [[nodiscard]] result update(const entity_type &entity) const {
      require_mutable_versioned_update();
      if constexpr (detail::is_joined_subtype_v<entity_type>) {
        return execute_owned([&](transaction &owner) {
          return joined_update(entity, std::nullopt, owner);
        });
      } else {
        return make_update(entity, std::nullopt).execute();
      }
    }

    [[nodiscard]] result update(const entity_type &entity, transaction &owner) const {
      require_mutable_versioned_update();
      if constexpr (detail::is_joined_subtype_v<entity_type>) {
        return joined_update(entity, std::nullopt, owner);
      } else {
        return make_update(entity, std::nullopt).execute(owner);
      }
    }

    template <typename Id> [[nodiscard]] result delete_by_id(Id &&id) const {
      if constexpr (detail::is_joined_subtype_v<entity_type>) {
        return execute_owned([&](transaction &owner) {
          return joined_delete(id, std::nullopt, owner);
        });
      } else {
        return make_delete(std::forward<Id>(id)).execute();
      }
    }

    template <typename Id>
    [[nodiscard]] result delete_by_id(Id &&id, transaction &owner) const {
      if constexpr (detail::is_joined_subtype_v<entity_type>) {
        return joined_delete(id, std::nullopt, owner);
      } else {
        return make_delete(std::forward<Id>(id)).execute(owner);
      }
    }

  private:
    template <typename> friend struct detail::managed_entry;

    inline static constexpr auto primary_key_names =
        model::get_primary_keys<entity_type>();
    inline static constexpr std::string_view version_name =
        model::get_version<entity_type>();

    [[nodiscard]] static std::string_view table_name() {
      return model::get_name<entity_type>();
    }

    static void require_primary_key() {
      std::vector<std::size_t> matches(primary_key_names.size(), 0);
      detail::for_each_flat_field_name<entity_type>([&](std::string_view name) {
        const auto found =
            std::find(primary_key_names.begin(), primary_key_names.end(), name);
        if (found != primary_key_names.end()) {
          ++matches[static_cast<std::size_t>(
              std::distance(primary_key_names.begin(), found))];
        }
      });
      if (matches.empty() ||
          std::any_of(matches.begin(), matches.end(),
                      [](std::size_t count) { return count != 1; }))
        throw std::invalid_argument(
            "ORM repository primary-key metadata does not match modeled fields");
    }

    [[nodiscard]] static bool is_primary_key(std::string_view name) {
      return std::find(primary_key_names.begin(), primary_key_names.end(), name) !=
             primary_key_names.end();
    }

    template <typename Target, typename Id>
    static void apply_primary_key(Target &target, const Id &id,
                                  bool qualified = false) {
      detail::for_each_primary_key_input<entity_type>(
          id, [&](std::string_view name, const auto &component) {
            std::string column_name(name);
            if (qualified) {
              column_name = std::string(table_name()) + "." + column_name;
            }
            target.where(column_name, comparison::equal,
                         detail::model_value(component));
          });
    }

    template <typename Id> [[nodiscard]] select_query select_by_id(Id &&id) const {
      require_primary_key();
      auto target = connection_->template select<entity_type>();
      apply_primary_key(target, id, detail::is_joined_entity_v<entity_type>);
      target.limit(1);
      return target;
    }

    [[nodiscard]] query make_insert(const entity_type &entity) const {
      query target = connection_->insert(table_name());
      detail::for_each_flat_field(entity, [&](std::string_view name, const auto &field) {
        target.set(name, detail::model_value(field));
      });
      apply_discriminator(target, true);
      return target;
    }

    static void apply_discriminator(query &target, bool assignment) {
      if constexpr (model::has_discriminator_metadata_v<entity_type>) {
        const auto column = model::get_discriminator_column<entity_type>();
        const auto discriminator = detail::model_value(
            model::get_discriminator_value<entity_type>());
        if (assignment)
          target.set(column, discriminator);
        else
          target.where(column, comparison::equal, discriminator);
      }
    }

    static void require_mutable_versioned_update() {
      if constexpr (detail::is_versioned_entity_v<entity_type>)
        throw std::invalid_argument(
            "ORM versioned repository update requires a mutable entity");
    }

    static void require_updated_one(const result &output) {
      if (output.affected_rows() != 1)
        throw status_error(
            ORM_STATUS_INVALID_STATE,
            "ORM optimistic lock conflict: entity version is stale or the row is missing");
    }

    [[nodiscard]] result execute_update(entity_type &entity,
                                        transaction *owner) const {
      if constexpr (!detail::is_versioned_entity_v<entity_type>) {
        if constexpr (detail::is_joined_subtype_v<entity_type>) {
          if (owner == nullptr)
            return execute_owned([&](transaction &joined_owner) {
              return joined_update(entity, std::nullopt, joined_owner);
            });
          return joined_update(entity, std::nullopt, *owner);
        } else {
          if (owner == nullptr) return make_update(entity, std::nullopt).execute();
          return make_update(entity, std::nullopt).execute(*owner);
        }
      } else {
        static_assert(std::is_copy_constructible_v<entity_type> &&
                          std::is_copy_assignable_v<entity_type>,
                      "ORM versioned repository entities must be copyable");
        entity_type previous(entity);
        const value expected = detail::version_value(previous);
        try {
          detail::increment_version(entity);
          result output = [&]() -> result {
            if constexpr (detail::is_joined_subtype_v<entity_type>) {
              if (owner == nullptr)
                return execute_owned([&](transaction &joined_owner) {
                  return joined_update(entity, expected, joined_owner);
                });
              return joined_update(entity, expected, *owner);
            } else {
              return owner == nullptr
                         ? make_update(entity, expected).execute()
                         : make_update(entity, expected).execute(*owner);
            }
          }();
          require_updated_one(output);
          return output;
        } catch (...) {
          entity = std::move(previous);
          throw;
        }
      }
    }

    [[nodiscard]] result update_prepared(const entity_type &entity,
                                         const entity_type &original,
                                         transaction &owner) const {
      if constexpr (detail::is_joined_subtype_v<entity_type>) {
        result output =
            joined_update(entity, detail::version_value(original), owner);
        require_updated_one(output);
        return output;
      } else {
        result output =
            make_update(entity, detail::version_value(original)).execute(owner);
        require_updated_one(output);
        return output;
      }
    }

    [[nodiscard]] result remove_prepared(const entity_type &entity,
                                         const entity_type &original,
                                         transaction &owner) const {
      if constexpr (detail::is_joined_subtype_v<entity_type>) {
        result output = joined_delete(detail::primary_key_id(entity),
                                      detail::version_value(original), owner);
        require_updated_one(output);
        return output;
      } else {
        query target = make_delete(detail::primary_key_id(entity));
        target.where(version_name, comparison::equal,
                     detail::version_value(original));
        result output = target.execute(owner);
        require_updated_one(output);
        return output;
      }
    }

    template <typename Operation>
    [[nodiscard]] result execute_owned(Operation &&operation) const {
      auto owner = connection_->begin_transaction();
      try {
        result output = std::forward<Operation>(operation)(owner);
        owner.commit();
        return output;
      } catch (...) {
        try {
          owner.rollback();
        } catch (...) {
        }
        throw;
      }
    }

    static void require_joined_fragment(const result &output) {
      if (output.affected_rows() != 1)
        throw status_error(
            ORM_STATUS_INVALID_STATE,
            "ORM joined inheritance fragment did not affect exactly one row");
    }

    [[nodiscard]] result joined_insert(const entity_type &entity,
                                       transaction &owner) const {
      constexpr auto tables =
          model::entity_model<entity_type>::inheritance_tables();
      constexpr auto column_tables =
          model::entity_model<entity_type>::column_tables();
      std::optional<result> output;
      for (const std::string_view fragment : tables) {
        query target = connection_->insert(fragment);
        std::size_t column_index = 0;
        detail::for_each_flat_field(
            entity, [&](std::string_view name, const auto &field) {
              if (column_tables[column_index++] == fragment)
                target.set(name, detail::model_value(field));
            });
        if (fragment != tables.front()) {
          detail::for_each_primary_key_input<entity_type>(
              detail::primary_key_id(entity),
              [&](std::string_view name, const auto &component) {
                target.set(name, detail::model_value(component));
              });
        } else {
          apply_discriminator(target, true);
        }
        output.emplace(target.execute(owner));
        require_joined_fragment(*output);
      }
      return std::move(*output);
    }

    [[nodiscard]] result joined_update(
        const entity_type &entity, std::optional<value> expected_version,
        transaction &owner) const {
      constexpr auto tables =
          model::entity_model<entity_type>::inheritance_tables();
      constexpr auto column_tables =
          model::entity_model<entity_type>::column_tables();
      std::optional<result> output;
      for (const std::string_view fragment : tables) {
        query target = connection_->update(fragment);
        bool has_assignment = false;
        std::size_t column_index = 0;
        detail::for_each_flat_field(
            entity, [&](std::string_view name, const auto &field) {
              if (column_tables[column_index++] == fragment &&
                  !is_primary_key(name)) {
                target.set(name, detail::model_value(field));
                has_assignment = true;
              }
            });
        if (!has_assignment) {
          detail::for_each_primary_key_input<entity_type>(
              detail::primary_key_id(entity),
              [&](std::string_view name, const auto &component) {
                if (!has_assignment) {
                  target.set(name, detail::model_value(component));
                  has_assignment = true;
                }
              });
        }
        apply_primary_key(target, detail::primary_key_id(entity));
        if (fragment == tables.front()) {
          apply_discriminator(target, false);
          if (expected_version.has_value())
            target.where(version_name, comparison::equal,
                         std::move(*expected_version));
        }
        output.emplace(target.execute(owner));
        require_joined_fragment(*output);
      }
      return std::move(*output);
    }

    template <typename Id>
    [[nodiscard]] result joined_delete(
        const Id &id, std::optional<value> expected_version,
        transaction &owner) const {
      constexpr auto tables =
          model::entity_model<entity_type>::inheritance_tables();
      std::optional<result> output;
      for (std::size_t index = tables.size(); index != 0; --index) {
        const std::string_view fragment = tables[index - 1];
        query target = connection_->delete_from(fragment);
        apply_primary_key(target, id);
        if (fragment == tables.front()) {
          apply_discriminator(target, false);
          if (expected_version.has_value())
            target.where(version_name, comparison::equal,
                         std::move(*expected_version));
        }
        output.emplace(target.execute(owner));
        require_joined_fragment(*output);
      }
      return std::move(*output);
    }

    [[nodiscard]] query make_update(
        const entity_type &entity,
        std::optional<value> expected_version) const {
      require_primary_key();
      query target = connection_->update(table_name());
      bool has_assignment = false;
      detail::for_each_flat_field(entity, [&](std::string_view name, const auto &field) {
        if (!is_primary_key(name)) {
          target.set(name, detail::model_value(field));
          has_assignment = true;
        }
      });
      if (!has_assignment)
        throw std::invalid_argument("ORM repository update has no non-primary-key fields");
      apply_primary_key(target, detail::primary_key_id(entity));
      apply_discriminator(target, false);
      if (expected_version.has_value())
        target.where(version_name, comparison::equal,
                     std::move(*expected_version));
      return target;
    }

    template <typename Id> [[nodiscard]] query make_delete(Id &&id) const {
      require_primary_key();
      query target = connection_->delete_from(table_name());
      apply_primary_key(target, id);
      apply_discriminator(target, false);
      return target;
    }

    const connection *connection_;
  };

  template <typename Root> class polymorphic_repository final {
  public:
    using root_type = std::remove_cv_t<std::remove_reference_t<Root>>;
    using model_type = model::entity_model<root_type>;
    using entity_type = typename model_type::polymorphic_type;

    static_assert(model::has_inheritance_hierarchy_v<root_type>,
                  "ORM polymorphic_repository<T> requires generated inheritance metadata");

    explicit polymorphic_repository(const connection &owner) noexcept
        : connection_(&owner) {}

    [[nodiscard]] std::vector<entity_type> find_all() const {
      if constexpr (model::has_discriminator_metadata_v<root_type>)
        require_known_discriminators(nullptr);
      std::vector<entity_type> output;
      std::apply(
          [&](const auto &...entry) { (append_all(output, entry, nullptr), ...); },
          model_type::discriminator_cases());
      return output;
    }

    [[nodiscard]] std::vector<entity_type> find_all(transaction &owner) const {
      if constexpr (model::has_discriminator_metadata_v<root_type>)
        require_known_discriminators(&owner);
      std::vector<entity_type> output;
      std::apply(
          [&](const auto &...entry) { (append_all(output, entry, &owner), ...); },
          model_type::discriminator_cases());
      return output;
    }

    template <typename Id>
    [[nodiscard]] std::optional<entity_type> find_by_id(const Id &id) const {
      return find_by_id_impl(id, nullptr);
    }

    template <typename Id>
    [[nodiscard]] std::optional<entity_type> find_by_id(
        const Id &id, transaction &owner) const {
      return find_by_id_impl(id, &owner);
    }

    [[nodiscard]] result insert(const entity_type &entity) const {
      return std::visit(
          [&](const auto &value) {
            using concrete_type = std::decay_t<decltype(value)>;
            return repository<concrete_type>(*connection_).insert(value);
          },
          entity);
    }

    [[nodiscard]] result insert(const entity_type &entity,
                                transaction &owner) const {
      return std::visit(
          [&](const auto &value) {
            using concrete_type = std::decay_t<decltype(value)>;
            return repository<concrete_type>(*connection_).insert(value, owner);
          },
          entity);
    }

    [[nodiscard]] result update(entity_type &entity) const {
      return std::visit(
          [&](auto &value) {
            using concrete_type = std::decay_t<decltype(value)>;
            return repository<concrete_type>(*connection_).update(value);
          },
          entity);
    }

    [[nodiscard]] result update(entity_type &entity,
                                transaction &owner) const {
      return std::visit(
          [&](auto &value) {
            using concrete_type = std::decay_t<decltype(value)>;
            return repository<concrete_type>(*connection_).update(value, owner);
          },
          entity);
    }

    [[nodiscard]] result remove(const entity_type &entity) const {
      return std::visit(
          [&](const auto &value) {
            using concrete_type = std::decay_t<decltype(value)>;
            return repository<concrete_type>(*connection_).delete_by_id(
                detail::primary_key_id(value));
          },
          entity);
    }

    [[nodiscard]] result remove(const entity_type &entity,
                                transaction &owner) const {
      return std::visit(
          [&](const auto &value) {
            using concrete_type = std::decay_t<decltype(value)>;
            return repository<concrete_type>(*connection_).delete_by_id(
                detail::primary_key_id(value), owner);
          },
          entity);
    }

  private:
    void require_known_discriminators(transaction *owner) const {
      query target = connection_->select(model::get_name<root_type>());
      target.column(model::get_discriminator_column<root_type>());
      using discriminator_row = std::tuple<std::string>;
      const auto rows = owner == nullptr
                            ? target.template fetch<discriminator_row>()
                            : target.template fetch<discriminator_row>(*owner);
      for (const auto &row : rows) {
        const std::string &value = std::get<0>(row);
        bool recognized = false;
        std::apply(
            [&](const auto &...entry) {
              ((recognized = recognized || value == entry.value), ...);
            },
            model_type::discriminator_cases());
        if (!recognized)
          throw status_error(
              ORM_STATUS_DATASTORE_ERROR,
              "ORM row contains an unknown inheritance discriminator value");
      }
    }

    template <typename Case>
    void append_all(std::vector<entity_type> &output, const Case &,
                    transaction *owner) const {
      using concrete_type = typename Case::entity_type;
      repository<concrete_type> target(*connection_);
      auto rows = owner == nullptr ? target.find_all() : target.find_all(*owner);
      output.reserve(output.size() + rows.size());
      for (auto &row : rows) {
        output.emplace_back(std::in_place_type<concrete_type>, std::move(row));
      }
    }

    template <typename Id>
    [[nodiscard]] std::optional<entity_type> find_by_id_impl(
        const Id &id, transaction *owner) const {
      if constexpr (!model::has_discriminator_metadata_v<root_type>) {
        std::optional<entity_type> output;
        std::apply(
            [&](const auto &...entry) {
              (load_unique_case(output, id, owner, entry), ...);
            },
            model_type::discriminator_cases());
        return output;
      } else {
      query target = connection_->select(model::get_name<root_type>());
      target.column(model::get_discriminator_column<root_type>());
      detail::for_each_primary_key_input<root_type>(
          id, [&](std::string_view name, const auto &component) {
            target.where(name, comparison::equal,
                         detail::model_value(component));
          });
      target.limit(1);
      using discriminator_row = std::tuple<std::string>;
      auto discriminator_rows =
          owner == nullptr
              ? target.template fetch<discriminator_row>()
              : target.template fetch<discriminator_row>(*owner);
      if (discriminator_rows.empty()) return std::nullopt;
      const std::string& discriminator =
          std::get<0>(discriminator_rows.front());

      std::optional<entity_type> output;
      bool recognized = false;
      std::apply(
          [&](const auto &...entry) {
            (load_case(output, recognized, discriminator, id, owner, entry),
             ...);
          },
          model_type::discriminator_cases());
      if (!recognized)
        throw status_error(
            ORM_STATUS_DATASTORE_ERROR,
            "ORM row contains an unknown inheritance discriminator value");
      if (!output.has_value())
        throw status_error(
            ORM_STATUS_DATASTORE_ERROR,
            "ORM polymorphic row disappeared during materialization");
      return output;
      }
    }

    template <typename Id, typename Case>
    void load_unique_case(std::optional<entity_type> &output, const Id &id,
                          transaction *owner, const Case &) const {
      using concrete_type = typename Case::entity_type;
      repository<concrete_type> target(*connection_);
      auto row = owner == nullptr ? target.find_by_id(id)
                                  : target.find_by_id(id, *owner);
      if (!row.has_value()) return;
      if (output.has_value())
        throw status_error(
            ORM_STATUS_DATASTORE_ERROR,
            "ORM table-per-class hierarchy contains a duplicate primary key");
      output.emplace(std::in_place_type<concrete_type>, std::move(*row));
    }

    template <typename Id, typename Case>
    void load_case(std::optional<entity_type> &output, bool &recognized,
                   const std::string &discriminator, const Id &id,
                   transaction *owner, const Case &entry) const {
      if (recognized || discriminator != entry.value) return;
      recognized = true;
      using concrete_type = typename Case::entity_type;
      repository<concrete_type> target(*connection_);
      auto row = owner == nullptr ? target.find_by_id(id)
                                  : target.find_by_id(id, *owner);
      if (row.has_value()) {
        output.emplace(std::in_place_type<concrete_type>, std::move(*row));
      }
    }

    const connection *connection_;
  };

  namespace detail {

    template <typename Entity, typename = void>
    struct has_lifecycle_dispatch : std::false_type {};

    template <typename Entity>
    struct has_lifecycle_dispatch<
        Entity,
        std::void_t<decltype(model::entity_model<Entity>::lifecycle(
            std::declval<model::lifecycle_event>(),
            std::declval<Entity &>()))>> : std::true_type {};

    template <typename Entity>
    void invoke_lifecycle(model::lifecycle_event event, Entity &entity) {
      if constexpr (has_lifecycle_dispatch<Entity>::value) {
        static_assert(
            std::is_same_v<void, decltype(model::entity_model<Entity>::lifecycle(
                                     event, entity))>,
            "ORM entity lifecycle dispatch must return void");
        model::entity_model<Entity>::lifecycle(event, entity);
      }
    }

    struct session_lifetime {
      session *owner = nullptr;
    };

    enum class managed_state : std::uint8_t { clean, added, removed };

    struct managed_entry_base {
      virtual ~managed_entry_base() = default;
      [[nodiscard]] virtual bool dirty() const = 0;
      [[nodiscard]] virtual bool added() const noexcept = 0;
      [[nodiscard]] virtual bool removed() const noexcept = 0;
      [[nodiscard]] virtual std::size_t depth() const noexcept = 0;
      [[nodiscard]] virtual const void *object_address() const noexcept = 0;
      virtual void mark_removed(std::size_t relation_depth) = 0;
      virtual void prepare() = 0;
      virtual void flush(transaction &owner) = 0;
      virtual void accept() noexcept = 0;
      virtual void reject() noexcept = 0;
      virtual void discard() = 0;
      [[nodiscard]] virtual std::function<void()> make_restore_action() = 0;
    };

    template <typename Entity> struct managed_entry final : managed_entry_base {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;

      explicit managed_entry(entity_type value, managed_state initial_state = managed_state::clean,
                             std::size_t relation_depth = 0)
          : current(std::move(value)), original(std::make_shared<const entity_type>(current)),
            identity(identity_key_for_entity(current)), state(initial_state),
            relation_depth(relation_depth) {
        static_assert(std::is_copy_constructible_v<entity_type>,
                      "ORM session entities must be copy constructible");
        static_assert(std::is_copy_assignable_v<entity_type>,
                      "ORM session entities must be copy assignable");
      }

      [[nodiscard]] bool dirty() const override {
        return state != managed_state::clean || !model_equal(current, *original);
      }

      [[nodiscard]] bool added() const noexcept override {
        return state == managed_state::added;
      }

      [[nodiscard]] bool removed() const noexcept override {
        return state == managed_state::removed;
      }

      [[nodiscard]] std::size_t depth() const noexcept override { return relation_depth; }

      [[nodiscard]] const void *object_address() const noexcept override {
        return &current;
      }

      void mark_removed(std::size_t requested_depth) override {
        if (state == managed_state::added)
          throw status_error(ORM_STATUS_INVALID_STATE,
                             "ORM session cannot remove an entity pending insertion");
        state = managed_state::removed;
        relation_depth = std::max(relation_depth, requested_depth);
      }

      void prepare() override {
        if (state == managed_state::added)
          invoke_lifecycle(model::lifecycle_event::pre_persist, current);
        else if (state == managed_state::removed)
          invoke_lifecycle(model::lifecycle_event::pre_remove, current);
        else
          invoke_lifecycle(model::lifecycle_event::pre_update, current);
        if (!(identity_key_for_entity(current) == identity))
          throw status_error(ORM_STATUS_INVALID_STATE,
                             "ORM lifecycle callback cannot change the primary key");
        if constexpr (is_versioned_entity_v<entity_type>) {
          if (state != managed_state::added &&
              version_token(current) != version_token(*original))
            throw status_error(
                ORM_STATUS_INVALID_STATE,
                "ORM managed entity version cannot be changed directly");
          if (state == managed_state::clean) {
            increment_version(current);
            version_incremented = true;
          }
        }
        pending = std::make_shared<const entity_type>(current);
      }

      void flush(transaction &owner) override {
        if (!(identity_key_for_entity(current) == identity))
          throw status_error(ORM_STATUS_INVALID_STATE,
                             "ORM session entity primary key cannot be changed");
        repository<entity_type> target(*connection);
        if (state == managed_state::added) {
          (void)target.insert(current, owner);
          invoke_lifecycle(model::lifecycle_event::post_persist, current);
        } else if (state == managed_state::removed) {
          if constexpr (is_versioned_entity_v<entity_type>)
            (void)target.remove_prepared(current, *original, owner);
          else
            (void)target.delete_by_id(primary_key_id(current), owner);
          invoke_lifecycle(model::lifecycle_event::post_remove, current);
        } else {
          if constexpr (is_versioned_entity_v<entity_type>)
            (void)target.update_prepared(current, *original, owner);
          else
            (void)target.update(current, owner);
          invoke_lifecycle(model::lifecycle_event::post_update, current);
        }
        if (!(identity_key_for_entity(current) == identity))
          throw status_error(ORM_STATUS_INVALID_STATE,
                             "ORM lifecycle callback cannot change the primary key");
      }

      void accept() noexcept override {
        if (state == managed_state::removed) {
          pending.reset();
        } else {
          original = std::move(pending);
          state = managed_state::clean;
        }
        version_incremented = false;
      }

      void reject() noexcept override {
        if constexpr (is_versioned_entity_v<entity_type>) {
          if (version_incremented) decrement_version_noexcept(current);
        }
        version_incremented = false;
        pending.reset();
      }

      void discard() override {
        current = *original;
        if (state == managed_state::removed) state = managed_state::clean;
      }

      void refresh(entity_type value) {
        auto snapshot = std::make_shared<const entity_type>(value);
        current = std::move(value);
        original = std::move(snapshot);
        pending.reset();
        state = managed_state::clean;
        version_incremented = false;
      }

      [[nodiscard]] std::function<void()> make_restore_action() override {
        entity_type current_snapshot = current;
        auto original_snapshot = original;
        auto pending_snapshot = pending;
        const managed_state state_snapshot = state;
        const std::size_t depth_snapshot = relation_depth;
        const bool version_snapshot = version_incremented;
        return [this, current_snapshot = std::move(current_snapshot),
                original_snapshot = std::move(original_snapshot),
                pending_snapshot = std::move(pending_snapshot), state_snapshot,
                depth_snapshot, version_snapshot]() mutable {
          current = std::move(current_snapshot);
          original = std::move(original_snapshot);
          pending = std::move(pending_snapshot);
          state = state_snapshot;
          relation_depth = depth_snapshot;
          version_incremented = version_snapshot;
        };
      }

      const connection *connection = nullptr;
      entity_type current;
      std::shared_ptr<const entity_type> original;
      std::shared_ptr<const entity_type> pending;
      identity_key identity;
      managed_state state;
      std::size_t relation_depth;
      bool version_incremented = false;
    };

  } // namespace detail

  class session final {
  public:
    inline static constexpr std::size_t default_entity_limit = 4096;
    inline static constexpr std::size_t default_prefetch_batch_limit = 256;

    explicit session(const connection &owner,
                     std::size_t max_entities = default_entity_limit)
        : connection_(&owner), lifetime_(std::make_shared<detail::session_lifetime>()),
          max_entities_(max_entities) {
      if (max_entities_ == 0)
        throw std::invalid_argument("ORM session entity limit must be greater than zero");
      lifetime_->owner = this;
    }

    ~session() noexcept { lifetime_->owner = nullptr; }

    session(const session &) = delete;
    session &operator=(const session &) = delete;
    session(session &&) = delete;
    session &operator=(session &&) = delete;

    [[nodiscard]] bool transaction_active() const noexcept {
      return active_transaction_ != nullptr;
    }

    [[nodiscard]] bool is_rollback_only() const noexcept { return rollback_only_; }

    void set_rollback_only() {
      if (!transaction_active())
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM session has no active transaction");
      rollback_only_ = true;
    }

    template <typename Operation>
    auto transactional(
        Operation &&operation,
        transaction_propagation propagation = transaction_propagation::required,
        isolation_level isolation = isolation_level::serializable)
        -> std::invoke_result_t<Operation, session &> {
      using result_type = std::invoke_result_t<Operation, session &>;
      static_assert(std::is_invocable_v<Operation, session &>,
                    "ORM transactional operation must accept orm::session&");
      static_assert(std::is_void_v<result_type> ||
                        (!std::is_reference_v<result_type> &&
                         std::is_nothrow_move_constructible_v<result_type>),
                    "ORM transactional operation must return void or a nothrow-movable value");

      if (transaction_active()) {
        if (propagation == transaction_propagation::requires_new ||
            propagation == transaction_propagation::not_supported)
          throw status_error(
              ORM_STATUS_UNSUPPORTED,
              "ORM session cannot suspend an active connection transaction");
        if (propagation == transaction_propagation::never)
          throw status_error(ORM_STATUS_INVALID_STATE,
                             "ORM transaction propagation NEVER rejected an active transaction");
        return invoke_joined_transaction(std::forward<Operation>(operation));
      }

      if (propagation == transaction_propagation::mandatory)
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM transaction propagation MANDATORY requires an active transaction");
      if (propagation == transaction_propagation::supports ||
          propagation == transaction_propagation::not_supported ||
          propagation == transaction_propagation::never)
        return std::invoke(std::forward<Operation>(operation), *this);
      return invoke_owned_transaction(std::forward<Operation>(operation), isolation);
    }

    template <typename Entity, typename Id>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    load(Id &&id) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      static_assert(detail::has_model_v<entity_type>,
                    "ORM session<T> requires an entity model");
      static_assert(std::is_default_constructible_v<entity_type>,
                    "ORM session<T> requires a default-constructible entity type");
      using entry_type = detail::managed_entry<entity_type>;

      const auto key = detail::identity_key_for_id<entity_type>(id);
      const auto found = entries_.find(key);
      if (found != entries_.end()) {
        auto typed = std::dynamic_pointer_cast<entry_type>(found->second);
        if (!typed) throw std::logic_error("ORM session identity-map type mismatch");
        return std::shared_ptr<entity_type>(typed, &typed->current);
      }

      auto entity = find_row_by_id<entity_type>(std::forward<Id>(id));
      if (!entity.has_value()) return {};

      const auto entity_key = detail::identity_key_for_entity(*entity);
      if (!(entity_key == key))
        throw std::logic_error("ORM session loaded entity primary key does not match lookup key");
      return adopt_loaded(std::move(*entity));
    }

    template <typename Entity, typename Id>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    find(Id &&id) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      if constexpr (detail::has_entity_graph_v<entity_type>) {
        graph_checkpoint checkpoint(*this);
        try {
          auto entity = load<Entity>(std::forward<Id>(id));
          if (entity)
            hydrate_graph(entity, model::entity_model<entity_type>::eager_graph());
          return entity;
        } catch (...) {
          checkpoint.restore(*this);
          throw;
        }
      } else {
        return load<Entity>(std::forward<Id>(id));
      }
    }

    template <typename Entity, typename Id>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    find(Id &&id,
         model::entity_graph<std::remove_cv_t<std::remove_reference_t<Entity>>> graph) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      static_assert(detail::has_entity_graph_v<entity_type>,
                    "ORM find<T>(id, graph) requires generated entity-graph metadata");
      graph_checkpoint checkpoint(*this);
      try {
        auto entity = load<Entity>(std::forward<Id>(id));
        if (entity) {
          hydrate_graph(entity,
                        model::entity_model<entity_type>::eager_graph() | graph);
        }
        return entity;
      } catch (...) {
        checkpoint.restore(*this);
        throw;
      }
    }

    template <typename Entity>
    void initialize(
        const std::shared_ptr<Entity> &entity,
        model::entity_graph<std::remove_cv_t<std::remove_reference_t<Entity>>> graph) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      static_assert(detail::has_entity_graph_v<entity_type>,
                    "ORM initialize<T> requires generated entity-graph metadata");
      if (!find_managed_entry(entity))
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM session cannot initialize a detached entity");
      hydrate_graph(entity, graph);
    }

    template <typename Entity, typename Id>
    [[nodiscard]] std::vector<std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>>
    load_many(const std::vector<Id> &ids,
              std::size_t batch_limit = default_prefetch_batch_limit) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      using entry_type = detail::managed_entry<entity_type>;
      static_assert(detail::has_model_v<entity_type>,
                    "ORM session load_many<T> requires an entity model");
      if (batch_limit == 0)
        throw std::invalid_argument("ORM prefetch batch limit must be greater than zero");
      if (ids.size() > batch_limit)
        throw status_error(ORM_STATUS_LIMIT_EXCEEDED,
                           "ORM prefetch batch exceeds the configured limit");
      if (ids.empty()) return {};

      std::vector<Id> missing;
      std::unordered_set<detail::identity_key, detail::identity_key_hash> missing_keys;
      for (const auto &id : ids) {
        const auto key = detail::identity_key_for_id<entity_type>(id);
        if (entries_.find(key) == entries_.end() && missing_keys.insert(key).second)
          missing.push_back(id);
      }

      if (!missing.empty()) {
        query target = connection_->select(model::get_name<entity_type>());
        detail::for_each_flat_field_name<entity_type>(
            [&](std::string_view name) { target.column(name); });
        const auto add_id_predicate = [&](const Id &id) {
          const bool composite =
              model::get_primary_keys<entity_type>().size() > 1;
          if (composite) target.begin_where(logic::and_);
          detail::for_each_primary_key_input<entity_type>(
              id, [&](std::string_view name, const auto &component) {
                target.where(name, comparison::equal,
                             detail::model_value(component));
              });
          if (composite) target.end_where();
        };
        if (missing.size() == 1) {
          add_id_predicate(missing.front());
        } else {
          target.begin_where(logic::or_);
          for (const auto &id : missing) add_id_predicate(id);
          target.end_where();
        }
        auto rows = fetch_rows<entity_type>(target);
        std::unordered_set<detail::identity_key, detail::identity_key_hash> row_keys;
        row_keys.reserve(rows.size());
        for (const auto &row : rows) {
          auto row_key = detail::identity_key_for_entity(row);
          if (missing_keys.find(row_key) == missing_keys.end())
            throw std::logic_error(
                "ORM session prefetch returned an unrequested primary key");
          if (!row_keys.insert(std::move(row_key)).second)
            throw std::logic_error("ORM session prefetch returned a duplicate primary key");
        }
        if (row_keys.size() > max_entities_ ||
            entries_.size() > max_entities_ - row_keys.size())
          throw status_error(ORM_STATUS_LIMIT_EXCEEDED,
                             "ORM session managed-entity limit exceeded");
        graph_checkpoint checkpoint(*this);
        try {
          for (auto &row : rows) (void)adopt_loaded(std::move(row));
        } catch (...) {
          checkpoint.restore(*this);
          throw;
        }
      }

      std::vector<std::shared_ptr<entity_type>> output;
      output.reserve(ids.size());
      for (const auto &id : ids) {
        const auto found = entries_.find(detail::identity_key_for_id<entity_type>(id));
        if (found == entries_.end()) continue;
        auto typed = std::dynamic_pointer_cast<entry_type>(found->second);
        if (!typed) throw std::logic_error("ORM session identity-map type mismatch");
        output.emplace_back(typed, &typed->current);
      }
      return output;
    }

    template <typename Entity>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    persist(Entity entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      if constexpr (model::has_relations_v<entity_type>) {
        auto managed = std::apply(
            [&](const auto &...relations) {
              return persist_graph(std::move(entity), relations...);
            },
            model::entity_model<entity_type>::relations());
        if constexpr (detail::has_entity_graph_v<entity_type>) {
          relation_masks_[detail::identity_key_for_entity(*managed)] =
              model::entity_model<entity_type>::all_graph().mask();
        }
        return managed;
      } else {
        return persist_at_depth(std::move(entity), 0);
      }
    }

    template <typename Entity>
    void remove(const std::shared_ptr<Entity> &entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      if constexpr (model::has_relations_v<entity_type>) {
        apply_model_graph_lifecycle<graph_lifecycle::remove>(entity);
      } else {
        remove_at_depth(entity, 0);
      }
    }

    template <typename Entity>
    [[nodiscard]] bool contains(const std::shared_ptr<Entity> &entity) const {
      return entity && find_managed_entry(entity) != nullptr;
    }

    template <typename Entity>
    [[nodiscard]] entity_state state(const std::shared_ptr<Entity> &entity) const {
      if (!entity) return entity_state::detached;
      const auto managed = find_managed_entry(entity);
      if (!managed) return entity_state::detached;
      if (managed->added()) return entity_state::added;
      if (managed->removed()) return entity_state::removed;
      return entity_state::managed;
    }

    template <typename Entity> void detach(const std::shared_ptr<Entity> &entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      if constexpr (model::has_relations_v<entity_type>) {
        apply_model_graph_lifecycle<graph_lifecycle::detach>(entity);
      } else {
        detach_one(entity);
      }
    }

    template <typename Entity>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    merge(const Entity &entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      if constexpr (model::has_relations_v<entity_type>) {
        auto managed = std::apply(
            [&](const auto &...relations) { return merge_graph(entity, relations...); },
            model::entity_model<entity_type>::relations());
        if constexpr (detail::has_entity_graph_v<entity_type>) {
          relation_masks_[detail::identity_key_for_entity(*managed)] =
              model::entity_model<entity_type>::all_graph().mask();
        }
        return managed;
      } else {
        return merge_at_depth(entity, 0);
      }
    }

    template <typename Entity> void refresh(const std::shared_ptr<Entity> &entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      if constexpr (model::has_relations_v<entity_type>) {
        apply_model_graph_lifecycle<graph_lifecycle::refresh>(entity);
      } else {
        refresh_one(entity);
      }
    }

    template <typename Entity, typename Id>
    void remove(Id &&id) {
      auto entity = load<Entity>(std::forward<Id>(id));
      if (!entity)
        throw status_error(ORM_STATUS_DATASTORE_ERROR,
                           "ORM session cannot remove a missing entity");
      remove_at_depth(entity, 0);
    }

    template <typename Entity, typename... Relations>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    persist_graph(Entity entity, const Relations &...relations) {
      graph_checkpoint checkpoint(*this);
      try {
        auto root = persist_at_depth(std::move(entity), 0);
        (cascade_persist(root, relations, 0), ...);
        (register_relation_tracker(root, relations), ...);
        return root;
      } catch (...) {
        checkpoint.restore(*this);
        throw;
      }
    }

    template <typename Entity, typename... Relations>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    merge_graph(const Entity &entity, const Relations &...relations) {
      graph_checkpoint checkpoint(*this);
      try {
        auto root = merge_at_depth(entity, 0);
        (cascade_merge(*root, relations, 0), ...);
        (register_relation_tracker(root, relations), ...);
        return root;
      } catch (...) {
        checkpoint.restore(*this);
        throw;
      }
    }

    template <typename Entity, typename... Relations>
    void refresh_graph(const std::shared_ptr<Entity> &root,
                       const Relations &...relations) {
      if (!root)
        throw std::invalid_argument("ORM session cannot cascade-refresh a null entity");
      graph_checkpoint checkpoint(*this);
      try {
        refresh_one(root);
        (cascade_refresh(*root, relations, 0), ...);
        rebase_relation_trackers(root.get());
      } catch (...) {
        checkpoint.restore(*this);
        throw;
      }
    }

    template <typename Entity, typename... Relations>
    void detach_graph(const std::shared_ptr<Entity> &root,
                      const Relations &...relations) {
      if (!root)
        throw std::invalid_argument("ORM session cannot cascade-detach a null entity");
      const auto managed_root = find_managed_entry(root);
      if (!managed_root) return;

      std::vector<std::shared_ptr<detail::managed_entry_base>> detach_plan;
      (collect_cascade_detach(*root, relations, 0, detach_plan), ...);
      detach_plan.push_back(managed_root);
      std::unordered_set<const detail::managed_entry_base *> seen;
      for (const auto &entry : detach_plan) {
        if (seen.insert(entry.get()).second) detach_entry(entry);
      }
    }

    template <typename Entity, typename... Relations>
    void remove_graph(const std::shared_ptr<Entity> &root, const Relations &...relations) {
      if (!root) throw std::invalid_argument("ORM session cannot cascade-remove a null entity");
      graph_checkpoint checkpoint(*this);
      try {
        (cascade_remove(root, relations, 0), ...);
        remove_at_depth(root, 0);
      } catch (...) {
        checkpoint.restore(*this);
        throw;
      }
    }

    template <typename Entity, typename ForeignKey>
    [[nodiscard]]
        std::vector<std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>>
    find_many(std::string_view foreign_key, const ForeignKey &value) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      static_assert(detail::has_model_v<entity_type>,
                    "ORM session find_many<T> requires an entity model");
      if (foreign_key.empty())
        throw std::invalid_argument("ORM session relation foreign-key column is empty");

      graph_checkpoint checkpoint(*this);
      try {
        auto target = connection_->template select<entity_type>();
        target.where(foreign_key, comparison::equal, detail::model_value(value));
        auto rows = fetch_rows<entity_type>(target);
        std::vector<std::shared_ptr<entity_type>> output;
        output.reserve(rows.size());
        for (auto &row : rows) {
          auto entity = adopt_loaded(std::move(row));
          if constexpr (detail::has_entity_graph_v<entity_type>) {
            hydrate_graph(entity,
                          model::entity_model<entity_type>::eager_graph());
          }
          output.push_back(std::move(entity));
        }
        return output;
      } catch (...) {
        checkpoint.restore(*this);
        throw;
      }
    }

    template <typename Entity, typename ForeignKey>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    find_one(std::string_view foreign_key, const ForeignKey &value) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      static_assert(detail::has_model_v<entity_type>,
                    "ORM session find_one<T> requires an entity model");
      if (foreign_key.empty())
        throw std::invalid_argument("ORM session relation foreign-key column is empty");

      graph_checkpoint checkpoint(*this);
      try {
        auto target = connection_->template select<entity_type>();
        target.where(foreign_key, comparison::equal, detail::model_value(value));
        auto rows = fetch_rows<entity_type>(target);
        if (rows.size() > 1)
          throw status_error(
              ORM_STATUS_DATASTORE_ERROR,
              "ORM to-one relation query returned more than one entity");
        if (rows.empty()) return {};
        auto entity = adopt_loaded(std::move(rows.front()));
        if constexpr (detail::has_entity_graph_v<entity_type>) {
          hydrate_graph(entity,
                        model::entity_model<entity_type>::eager_graph());
        }
        return entity;
      } catch (...) {
        checkpoint.restore(*this);
        throw;
      }
    }

    template <typename Entity, typename Id>
    [[nodiscard]] lazy_one<std::remove_cv_t<std::remove_reference_t<Entity>>, std::decay_t<Id>>
    defer(Id &&id);

    template <typename Entity, typename ForeignKey>
    [[nodiscard]]
        lazy_many<std::remove_cv_t<std::remove_reference_t<Entity>>, std::decay_t<ForeignKey>>
        defer_many(std::string_view foreign_key, ForeignKey &&value);

    template <typename Entity, typename ForeignKey>
    [[nodiscard]] lazy_one<std::remove_cv_t<std::remove_reference_t<Entity>>,
                           std::decay_t<ForeignKey>>
    defer_one(std::string_view foreign_key, ForeignKey &&value);

    [[nodiscard]] bool dirty() const noexcept {
      for (const auto &item : entries_)
        if (item.second->dirty()) return true;
      for (const auto &tracker : relation_trackers_)
        if (tracker->dirty()) return true;
      return false;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // Flush is an explicit unit of work. An active transactional scope owns commit.
    [[nodiscard]] std::size_t flush() {
      if (active_transaction_ != nullptr && rollback_only_)
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM transaction was marked rollback-only");
      std::optional<graph_checkpoint> checkpoint;
      if (!relation_trackers_.empty()) checkpoint.emplace(*this);
      flush_work work = prepare_flush(checkpoint);

      if (work.entries.empty()) {
        for (const auto &tracker : relation_trackers_) tracker->accept();
        return 0;
      }

      if (active_transaction_ != nullptr) {
        try {
          execute_flush(work, *active_transaction_);
          accept_flush(work);
        } catch (...) {
          reject_flush(work);
          rollback_only_ = true;
          throw;
        }
        return work.entries.size();
      }

      try {
        auto owner = connection_->begin_transaction();
        try {
          execute_flush(work, owner);
          owner.commit();
        } catch (...) {
          try {
            owner.rollback();
          } catch (...) {
          }
          throw;
        }
        accept_flush(work);
      } catch (...) {
        reject_flush(work);
        throw;
      }
      return work.entries.size();
    }

    void discard() {
      for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        if (iterator->second->added()) {
          relation_masks_.erase(iterator->first);
          iterator = entries_.erase(iterator);
        } else {
          iterator->second->discard();
          ++iterator;
        }
      }
      for (const auto &tracker : relation_trackers_) tracker->discard();
    }

    void clear() {
      if (dirty())
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM session cannot clear while managed entities are dirty");
      entries_.clear();
      relation_trackers_.clear();
      relation_masks_.clear();
    }

  private:
    inline static constexpr std::size_t max_cascade_depth = 64;

    template <typename Child>
    void synchronize_relation_copy(const Child &original, const Child &current) {
      if (!(detail::identity_key_for_entity(original) ==
            detail::identity_key_for_entity(current)) ||
          detail::model_equal(original, current))
        return;

      auto managed = find_managed_entry_by_value(current);
      if (!managed)
        throw status_error(
            ORM_STATUS_INVALID_STATE,
            "ORM materialized relation child is missing from the identity map");

      const bool canonical_changed =
          !detail::model_equal(managed->current, *managed->original);
      if (canonical_changed &&
          !detail::model_equal(managed->current, current))
        throw status_error(
            ORM_STATUS_INVALID_STATE,
            "ORM relation copy conflicts with its managed entity");

      detail::copy_modeled_fields(managed->current, current);
    }

    template <typename Owner, typename Child, typename Descriptor>
    std::shared_ptr<Child> attach_relation_child(
        Owner &owner_entity, Child &candidate, const Descriptor &descriptor) {
      auto managed = find_managed_entry_by_value(candidate);
      if (managed) {
        if (managed->removed())
          throw status_error(
              ORM_STATUS_INVALID_STATE,
              "ORM relation cannot attach an entity pending removal");
        const bool canonical_changed =
            !detail::model_equal(managed->current, *managed->original);
        if (canonical_changed &&
            !detail::model_equal(managed->current, candidate))
          throw status_error(
              ORM_STATUS_INVALID_STATE,
              "ORM relation candidate conflicts with its managed entity");
        detail::copy_modeled_fields(managed->current, candidate);
        if (descriptor.bind_foreign_key)
          descriptor.bind_foreign_key(owner_entity, managed->current);
        return std::shared_ptr<Child>(managed, &managed->current);
      }

      if (!relation::includes(descriptor.policy,
                              relation::cascade_policy::persist))
        throw status_error(
            ORM_STATUS_INVALID_STATE,
            "ORM relation replacement requires a managed child or cascade persist");

      if (descriptor.bind_foreign_key)
        descriptor.bind_foreign_key(owner_entity, candidate);
      auto persisted = persist_at_depth(candidate, 1);
      std::apply(
          [&](const auto &...nested) {
            (cascade_persist(persisted, nested, 1), ...);
          },
          descriptor.nested);
      return persisted;
    }

    template <typename Range, typename EntityOf>
    static std::unordered_set<detail::identity_key, detail::identity_key_hash>
    unique_relation_identities(const Range &children, EntityOf &&entity_of) {
      std::unordered_set<detail::identity_key, detail::identity_key_hash> identities;
      identities.reserve(children.size());
      for (const auto &item : children) {
        const auto &child = entity_of(item);
        if (!identities.emplace(detail::identity_key_for_entity(child)).second)
          throw status_error(
              ORM_STATUS_INVALID_STATE,
              "ORM relation collection contains a duplicate entity identity");
      }
      return identities;
    }

    struct relation_tracker_base {
      virtual ~relation_tracker_base() = default;
      [[nodiscard]] virtual detail::identity_key owner_key() const = 0;
      [[nodiscard]] virtual const void *owner_address() const noexcept = 0;
      [[nodiscard]] virtual bool dirty() const noexcept = 0;
      virtual void synchronize(session &owner) = 0;
      virtual void stage_current() = 0;
      virtual void accept() noexcept = 0;
      virtual void reject() noexcept = 0;
      virtual void discard() = 0;
    };

    template <typename Owner, typename Child, typename... Nested>
    struct value_one_relation_tracker final : relation_tracker_base {
      using descriptor_type = relation::has_one<Owner, Child, Nested...>;

      value_one_relation_tracker(const std::shared_ptr<Owner> &owner,
                                 const descriptor_type &descriptor)
          : owner_(owner), descriptor_(descriptor),
            original_(std::make_shared<const Child>((*owner).*descriptor.member)) {}

      [[nodiscard]] detail::identity_key owner_key() const override {
        return detail::identity_key_for_entity(*owner_);
      }

      [[nodiscard]] const void *owner_address() const noexcept override {
        return owner_.get();
      }

      [[nodiscard]] bool dirty() const noexcept override {
        try {
          return !detail::model_equal(*original_,
                                      (*owner_).*descriptor_.member);
        } catch (...) {
          return true;
        }
      }

      void synchronize(session &owner) override {
        const auto &current = (*owner_).*descriptor_.member;
        const auto old_key = detail::identity_key_for_entity(*original_);
        const auto new_key = detail::identity_key_for_entity(current);
        if (old_key == new_key) {
          owner.synchronize_relation_copy(*original_, current);
          stage_current();
          return;
        }
        auto &child = (*owner_).*descriptor_.member;
        auto managed = owner.attach_relation_child(*owner_, child, descriptor_);
        detail::copy_modeled_fields(child, *managed);
        if (relation::includes(descriptor_.policy,
                               relation::cascade_policy::orphan_remove)) {
          auto old = owner.adopt_loaded(*original_);
          owner.remove_at_depth(old, 1);
        }
        stage_current();
      }

      void accept() noexcept override { original_ = std::move(pending_); }
      void reject() noexcept override { pending_.reset(); }
      void stage_current() override {
        pending_ = std::make_shared<const Child>((*owner_).*descriptor_.member);
      }
      void discard() override { (*owner_).*descriptor_.member = *original_; }

      std::shared_ptr<Owner> owner_;
      descriptor_type descriptor_;
      std::shared_ptr<const Child> original_;
      std::shared_ptr<const Child> pending_;
    };

    template <typename Owner, typename Child, typename... Nested>
    struct optional_one_relation_tracker final : relation_tracker_base {
      using descriptor_type = relation::optional_one<Owner, Child, Nested...>;

      optional_one_relation_tracker(const std::shared_ptr<Owner> &owner,
                                    const descriptor_type &descriptor)
          : owner_(owner), descriptor_(descriptor),
            original_(std::make_shared<const std::optional<Child>>(
                (*owner).*descriptor.member)) {}

      [[nodiscard]] detail::identity_key owner_key() const override {
        return detail::identity_key_for_entity(*owner_);
      }

      [[nodiscard]] const void *owner_address() const noexcept override {
        return owner_.get();
      }

      [[nodiscard]] bool dirty() const noexcept override {
        try {
          return !detail::model_equal(*original_,
                                      (*owner_).*descriptor_.member);
        } catch (...) {
          return true;
        }
      }

      void synchronize(session &owner) override {
        const auto old_key = original_->has_value()
                                 ? std::optional<detail::identity_key>(
                                       detail::identity_key_for_entity(**original_))
                                 : std::nullopt;
        auto &new_key = (*owner_).*descriptor_.member;
        const auto new_identity = new_key.has_value()
                                      ? std::optional<detail::identity_key>(
                                            detail::identity_key_for_entity(*new_key))
                                      : std::nullopt;
        if (old_key == new_identity) {
          if (original_->has_value())
            owner.synchronize_relation_copy(**original_, *new_key);
          stage_current();
          return;
        }
        if (new_key.has_value()) {
          auto &child = *new_key;
          auto managed =
              owner.attach_relation_child(*owner_, child, descriptor_);
          detail::copy_modeled_fields(child, *managed);
        }
        if (original_->has_value() &&
            relation::includes(descriptor_.policy,
                               relation::cascade_policy::orphan_remove)) {
          auto old = owner.adopt_loaded(**original_);
          owner.remove_at_depth(old, 1);
        }
        stage_current();
      }

      void accept() noexcept override { original_ = std::move(pending_); }
      void reject() noexcept override { pending_.reset(); }
      void stage_current() override {
        pending_ = std::make_shared<const std::optional<Child>>(
            (*owner_).*descriptor_.member);
      }
      void discard() override { (*owner_).*descriptor_.member = *original_; }

      std::shared_ptr<Owner> owner_;
      descriptor_type descriptor_;
      std::shared_ptr<const std::optional<Child>> original_;
      std::shared_ptr<const std::optional<Child>> pending_;
    };

    template <typename Owner, typename Child, typename... Nested>
    struct shared_one_relation_tracker final : relation_tracker_base {
      using descriptor_type = relation::shared_one<Owner, Child, Nested...>;

      shared_one_relation_tracker(const std::shared_ptr<Owner> &owner,
                                  const descriptor_type &descriptor)
          : owner_(owner), descriptor_(descriptor),
            original_(std::make_shared<const std::shared_ptr<Child>>(
                (*owner).*descriptor.member)),
            original_value_(snapshot((*owner).*descriptor.member)) {}

      static std::shared_ptr<const std::optional<Child>>
      snapshot(const std::shared_ptr<Child> &value) {
        return std::make_shared<const std::optional<Child>>(
            value ? std::optional<Child>(*value) : std::nullopt);
      }

      [[nodiscard]] detail::identity_key owner_key() const override {
        return detail::identity_key_for_entity(*owner_);
      }

      [[nodiscard]] const void *owner_address() const noexcept override {
        return owner_.get();
      }

      [[nodiscard]] bool dirty() const noexcept override {
        try {
          const auto &current = (*owner_).*descriptor_.member;
          if (original_value_->has_value() != static_cast<bool>(current))
            return true;
          return original_value_->has_value() &&
                 !detail::model_equal(**original_value_, *current);
        } catch (...) {
          return true;
        }
      }

      void synchronize(session &owner) override {
        const auto old_key = *original_
                                 ? std::optional<detail::identity_key>(
                                       detail::identity_key_for_entity(**original_))
                                 : std::nullopt;
        auto &new_value = (*owner_).*descriptor_.member;
        const auto new_key = new_value
                                 ? std::optional<detail::identity_key>(
                                       detail::identity_key_for_entity(*new_value))
                                 : std::nullopt;
        if (old_key == new_key) {
          if (new_value) {
            const bool changed =
                !detail::model_equal(**original_value_, *new_value);
            if (changed)
              owner.synchronize_relation_copy(**original_value_, *new_value);
            auto managed = owner.find_managed_entry_by_value(*new_value);
            if (managed)
              new_value = std::shared_ptr<Child>(managed, &managed->current);
            else if (changed)
              throw status_error(
                  ORM_STATUS_INVALID_STATE,
                  "ORM materialized shared relation child is missing from the identity map");
          }
          stage_current();
          return;
        }
        if (new_value)
          new_value =
              owner.attach_relation_child(*owner_, *new_value, descriptor_);
        if (*original_ &&
            relation::includes(descriptor_.policy,
                               relation::cascade_policy::orphan_remove)) {
          auto old = owner.adopt_loaded(**original_);
          owner.remove_at_depth(old, 1);
        }
        stage_current();
      }

      void accept() noexcept override {
        original_ = std::move(pending_);
        original_value_ = std::move(pending_value_);
      }
      void reject() noexcept override {
        pending_.reset();
        pending_value_.reset();
      }
      void stage_current() override {
        pending_ = std::make_shared<const std::shared_ptr<Child>>(
            (*owner_).*descriptor_.member);
        pending_value_ = snapshot((*owner_).*descriptor_.member);
      }
      void discard() override {
        if (*original_ && original_value_->has_value())
          detail::copy_modeled_fields(**original_, **original_value_);
        (*owner_).*descriptor_.member = *original_;
      }

      std::shared_ptr<Owner> owner_;
      descriptor_type descriptor_;
      std::shared_ptr<const std::shared_ptr<Child>> original_;
      std::shared_ptr<const std::shared_ptr<Child>> pending_;
      std::shared_ptr<const std::optional<Child>> original_value_;
      std::shared_ptr<const std::optional<Child>> pending_value_;
    };

    template <typename Owner, typename Child, typename... Nested>
    struct value_many_relation_tracker final : relation_tracker_base {
      using descriptor_type = relation::has_many<Owner, Child, Nested...>;

      value_many_relation_tracker(const std::shared_ptr<Owner> &owner,
                                  const descriptor_type &descriptor)
          : owner_(owner), descriptor_(descriptor),
            original_(std::make_shared<const std::vector<Child>>(
                (*owner).*descriptor.member)) {}

      [[nodiscard]] detail::identity_key owner_key() const override {
        return detail::identity_key_for_entity(*owner_);
      }

      [[nodiscard]] const void *owner_address() const noexcept override {
        return owner_.get();
      }

      [[nodiscard]] bool dirty() const noexcept override {
        try {
          const auto &current = (*owner_).*descriptor_.member;
          if (original_->size() != current.size()) return true;
          for (const auto &before : *original_) {
            const auto key = detail::identity_key_for_entity(before);
            const auto found = std::find_if(
                current.begin(), current.end(), [&](const auto &candidate) {
                  return detail::identity_key_for_entity(candidate) == key;
                });
            if (found == current.end() ||
                !detail::model_equal(before, *found))
              return true;
          }
          return false;
        } catch (...) {
          return true;
        }
      }

      void synchronize(session &owner) override {
        auto current_keys = owner.unique_relation_identities(
            (*owner_).*descriptor_.member,
            [](const Child &child) -> const Child & { return child; });
        std::unordered_map<detail::identity_key, const Child *, detail::identity_key_hash>
            original_children;
        for (const auto &child : *original_)
          original_children.emplace(detail::identity_key_for_entity(child), &child);
        for (const auto &child : (*owner_).*descriptor_.member) {
          const auto found =
              original_children.find(detail::identity_key_for_entity(child));
          if (found != original_children.end())
            owner.synchronize_relation_copy(*found->second, child);
        }
        for (auto &child : (*owner_).*descriptor_.member) {
          if (original_children.find(detail::identity_key_for_entity(child)) !=
              original_children.end())
            continue;
          auto managed =
              owner.attach_relation_child(*owner_, child, descriptor_);
          detail::copy_modeled_fields(child, *managed);
        }
        if (relation::includes(descriptor_.policy,
                               relation::cascade_policy::orphan_remove)) {
          for (const auto &child : *original_) {
            if (current_keys.find(detail::identity_key_for_entity(child)) !=
                current_keys.end())
              continue;
            auto managed = owner.adopt_loaded(child);
            owner.remove_at_depth(managed, 1);
          }
        }
        stage_current();
      }

      void accept() noexcept override { original_ = std::move(pending_); }
      void reject() noexcept override { pending_.reset(); }
      void stage_current() override {
        pending_ = std::make_shared<const std::vector<Child>>(
            (*owner_).*descriptor_.member);
      }
      void discard() override { (*owner_).*descriptor_.member = *original_; }

      std::shared_ptr<Owner> owner_;
      descriptor_type descriptor_;
      std::shared_ptr<const std::vector<Child>> original_;
      std::shared_ptr<const std::vector<Child>> pending_;
    };

    template <typename Owner, typename Child, typename... Nested>
    struct shared_many_relation_tracker final : relation_tracker_base {
      using descriptor_type = relation::shared_many<Owner, Child, Nested...>;

      shared_many_relation_tracker(const std::shared_ptr<Owner> &owner,
                                   const descriptor_type &descriptor)
          : owner_(owner), descriptor_(descriptor),
            original_(std::make_shared<const std::vector<std::shared_ptr<Child>>>(
                (*owner).*descriptor.member)),
            original_values_(snapshot((*owner).*descriptor.member)) {}

      static std::shared_ptr<const std::vector<Child>>
      snapshot(const std::vector<std::shared_ptr<Child>> &values) {
        std::vector<Child> output;
        output.reserve(values.size());
        for (const auto &value : values) {
          if (!value)
            throw std::invalid_argument(
                "ORM relation collection contains a null entity");
          output.push_back(*value);
        }
        return std::make_shared<const std::vector<Child>>(std::move(output));
      }

      [[nodiscard]] detail::identity_key owner_key() const override {
        return detail::identity_key_for_entity(*owner_);
      }

      [[nodiscard]] const void *owner_address() const noexcept override {
        return owner_.get();
      }

      [[nodiscard]] bool dirty() const noexcept override {
        try {
          const auto &current = (*owner_).*descriptor_.member;
          if (original_values_->size() != current.size()) return true;
          for (const auto &before : *original_values_) {
            const auto key = detail::identity_key_for_entity(before);
            const auto found = std::find_if(
                current.begin(), current.end(), [&](const auto &candidate) {
                  return candidate &&
                         detail::identity_key_for_entity(*candidate) == key;
                });
            if (found == current.end() ||
                !detail::model_equal(before, **found))
              return true;
          }
          return false;
        } catch (...) {
          return true;
        }
      }

      void synchronize(session &owner) override {
        auto current_keys = owner.unique_relation_identities(
            (*owner_).*descriptor_.member,
            [](const std::shared_ptr<Child> &child) -> const Child & {
              if (!child)
                throw std::invalid_argument(
                    "ORM relation collection contains a null entity");
              return *child;
            });
        std::unordered_map<detail::identity_key, const Child *, detail::identity_key_hash>
            original_children;
        for (const auto &child : *original_values_) {
          original_children.emplace(detail::identity_key_for_entity(child),
                                    &child);
        }
        for (auto &child : (*owner_).*descriptor_.member) {
          const auto found =
              original_children.find(detail::identity_key_for_entity(*child));
          if (found == original_children.end()) continue;
          const bool changed = !detail::model_equal(*found->second, *child);
          if (changed)
            owner.synchronize_relation_copy(*found->second, *child);
          auto managed = owner.find_managed_entry_by_value(*child);
          if (managed)
            child = std::shared_ptr<Child>(managed, &managed->current);
          else if (changed)
            throw status_error(
                ORM_STATUS_INVALID_STATE,
                "ORM materialized shared relation child is missing from the identity map");
        }
        for (auto &child : (*owner_).*descriptor_.member) {
          if (original_children.find(detail::identity_key_for_entity(*child)) !=
              original_children.end())
            continue;
          child = owner.attach_relation_child(*owner_, *child, descriptor_);
        }
        if (relation::includes(descriptor_.policy,
                               relation::cascade_policy::orphan_remove)) {
          for (const auto &child : *original_values_) {
            if (current_keys.find(detail::identity_key_for_entity(child)) !=
                current_keys.end())
              continue;
            auto managed = owner.adopt_loaded(child);
            owner.remove_at_depth(managed, 1);
          }
        }
        stage_current();
      }

      void accept() noexcept override {
        original_ = std::move(pending_);
        original_values_ = std::move(pending_values_);
      }
      void reject() noexcept override {
        pending_.reset();
        pending_values_.reset();
      }
      void stage_current() override {
        pending_ =
            std::make_shared<const std::vector<std::shared_ptr<Child>>>(
                (*owner_).*descriptor_.member);
        pending_values_ = snapshot((*owner_).*descriptor_.member);
      }
      void discard() override {
        if (original_->size() != original_values_->size())
          throw std::logic_error("ORM shared relation snapshot size mismatch");
        for (std::size_t index = 0; index < original_->size(); ++index) {
          if (!(*original_)[index])
            throw std::logic_error(
                "ORM shared relation snapshot contains a null entity");
          detail::copy_modeled_fields(*(*original_)[index],
                                      (*original_values_)[index]);
        }
        (*owner_).*descriptor_.member = *original_;
      }

      std::shared_ptr<Owner> owner_;
      descriptor_type descriptor_;
      std::shared_ptr<const std::vector<std::shared_ptr<Child>>> original_;
      std::shared_ptr<const std::vector<std::shared_ptr<Child>>> pending_;
      std::shared_ptr<const std::vector<Child>> original_values_;
      std::shared_ptr<const std::vector<Child>> pending_values_;
    };

    template <typename Owner, typename Child, typename... Nested>
    void register_relation_tracker(
        const std::shared_ptr<Owner> &owner,
        const relation::has_many<Owner, Child, Nested...> &descriptor) {
      using tracker_type = value_many_relation_tracker<Owner, Child, Nested...>;
      for (const auto &tracker : relation_trackers_) {
        auto typed = dynamic_cast<tracker_type *>(tracker.get());
        if (typed && typed->owner_.get() == owner.get() &&
            typed->descriptor_.member == descriptor.member)
          return;
      }
      relation_trackers_.push_back(std::make_shared<tracker_type>(owner, descriptor));
    }

    template <typename Owner, typename Child, typename... Nested>
    void register_relation_tracker(
        const std::shared_ptr<Owner> &owner,
        const relation::shared_many<Owner, Child, Nested...> &descriptor) {
      using tracker_type = shared_many_relation_tracker<Owner, Child, Nested...>;
      for (const auto &tracker : relation_trackers_) {
        auto typed = dynamic_cast<tracker_type *>(tracker.get());
        if (typed && typed->owner_.get() == owner.get() &&
            typed->descriptor_.member == descriptor.member)
          return;
      }
      for (auto &child : (*owner).*descriptor.member) {
        if (!child)
          throw std::invalid_argument(
              "ORM relation collection contains a null entity");
        if (auto managed = find_managed_entry_by_value(*child))
          child = std::shared_ptr<Child>(managed, &managed->current);
      }
      relation_trackers_.push_back(std::make_shared<tracker_type>(owner, descriptor));
    }

    template <typename Owner, typename Child, typename... Nested>
    void register_relation_tracker(
        const std::shared_ptr<Owner> &owner,
        const relation::has_one<Owner, Child, Nested...> &descriptor) {
      using tracker_type = value_one_relation_tracker<Owner, Child, Nested...>;
      for (const auto &tracker : relation_trackers_) {
        auto typed = dynamic_cast<tracker_type *>(tracker.get());
        if (typed && typed->owner_.get() == owner.get() &&
            typed->descriptor_.member == descriptor.member)
          return;
      }
      relation_trackers_.push_back(std::make_shared<tracker_type>(owner, descriptor));
    }

    template <typename Owner, typename Child, typename... Nested>
    void register_relation_tracker(
        const std::shared_ptr<Owner> &owner,
        const relation::optional_one<Owner, Child, Nested...> &descriptor) {
      using tracker_type = optional_one_relation_tracker<Owner, Child, Nested...>;
      for (const auto &tracker : relation_trackers_) {
        auto typed = dynamic_cast<tracker_type *>(tracker.get());
        if (typed && typed->owner_.get() == owner.get() &&
            typed->descriptor_.member == descriptor.member)
          return;
      }
      relation_trackers_.push_back(std::make_shared<tracker_type>(owner, descriptor));
    }

    template <typename Owner, typename Child, typename... Nested>
    void register_relation_tracker(
        const std::shared_ptr<Owner> &owner,
        const relation::shared_one<Owner, Child, Nested...> &descriptor) {
      using tracker_type = shared_one_relation_tracker<Owner, Child, Nested...>;
      for (const auto &tracker : relation_trackers_) {
        auto typed = dynamic_cast<tracker_type *>(tracker.get());
        if (typed && typed->owner_.get() == owner.get() &&
            typed->descriptor_.member == descriptor.member)
          return;
      }
      auto &child = (*owner).*descriptor.member;
      if (child) {
        if (auto managed = find_managed_entry_by_value(*child))
          child = std::shared_ptr<Child>(managed, &managed->current);
      }
      relation_trackers_.push_back(std::make_shared<tracker_type>(owner, descriptor));
    }

    void rebase_relation_trackers(const void *owner_address) {
      try {
        for (const auto &tracker : relation_trackers_)
          if (tracker->owner_address() == owner_address)
            tracker->stage_current();
      } catch (...) {
        for (const auto &tracker : relation_trackers_)
          if (tracker->owner_address() == owner_address)
            tracker->reject();
        throw;
      }
      for (const auto &tracker : relation_trackers_)
        if (tracker->owner_address() == owner_address)
          tracker->accept();
    }

    using entry_map =
        std::unordered_map<detail::identity_key, std::shared_ptr<detail::managed_entry_base>,
                           detail::identity_key_hash>;
    using relation_mask_map =
        std::unordered_map<detail::identity_key, std::uint64_t,
                           detail::identity_key_hash>;

    enum class graph_lifecycle : std::uint8_t { remove, refresh, detach };

    struct graph_checkpoint final {
      explicit graph_checkpoint(const session &owner)
          : entries(owner.entries_), relation_trackers(owner.relation_trackers_),
            relation_masks(owner.relation_masks_) {
        restore_actions.reserve(owner.entries_.size());
        for (const auto &item : owner.entries_)
          restore_actions.push_back(item.second->make_restore_action());
      }

      void restore(session &owner) {
        for (auto &restore : restore_actions) restore();
        owner.entries_.swap(entries);
        owner.relation_trackers_.swap(relation_trackers);
        owner.relation_masks_.swap(relation_masks);
      }

      entry_map entries;
      std::vector<std::shared_ptr<relation_tracker_base>> relation_trackers;
      relation_mask_map relation_masks;
      std::vector<std::function<void()>> restore_actions;
    };

    struct flush_work final {
      std::vector<std::shared_ptr<detail::managed_entry_base>> entries;
    };

    template <typename Operation>
    auto invoke_joined_transaction(Operation &&operation)
        -> std::invoke_result_t<Operation, session &> {
      try {
        return std::invoke(std::forward<Operation>(operation), *this);
      } catch (...) {
        rollback_only_ = true;
        throw;
      }
    }

    template <typename Operation>
    auto invoke_owned_transaction(Operation &&operation, isolation_level isolation)
        -> std::invoke_result_t<Operation, session &> {
      using result_type = std::invoke_result_t<Operation, session &>;
      auto owner = connection_->begin_transaction(isolation);
      graph_checkpoint checkpoint(*this);
      active_transaction_ = &owner;
      rollback_only_ = false;

      if constexpr (std::is_void_v<result_type>) {
        try {
          std::invoke(std::forward<Operation>(operation), *this);
          finish_owned_transaction(owner);
          reset_transaction_state();
        } catch (...) {
          try {
            owner.rollback();
          } catch (...) {
          }
          reset_transaction_state();
          checkpoint.restore(*this);
          throw;
        }
        return;
      } else {
        std::optional<result_type> result;
        try {
          result.emplace(std::invoke(std::forward<Operation>(operation), *this));
          finish_owned_transaction(owner);
          reset_transaction_state();
        } catch (...) {
          try {
            owner.rollback();
          } catch (...) {
          }
          reset_transaction_state();
          checkpoint.restore(*this);
          throw;
        }
        return std::move(*result);
      }
    }

    void finish_owned_transaction(transaction &owner) {
      if (rollback_only_)
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM transaction was marked rollback-only");
      (void)flush();
      if (rollback_only_)
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM transaction was marked rollback-only");
      owner.commit();
    }

    void reset_transaction_state() noexcept {
      active_transaction_ = nullptr;
      rollback_only_ = false;
    }

    flush_work prepare_flush(std::optional<graph_checkpoint> &checkpoint) {
      flush_work work;
      try {
        for (const auto &tracker : relation_trackers_)
          tracker->synchronize(*this);
      } catch (...) {
        for (const auto &tracker : relation_trackers_) tracker->reject();
        if (checkpoint.has_value()) checkpoint->restore(*this);
        throw;
      }

      for (const auto &item : entries_)
        if (item.second->dirty()) work.entries.push_back(item.second);
      std::stable_sort(work.entries.begin(), work.entries.end(),
                       [](const auto &left, const auto &right) {
                         if (left->added() != right->added())
                           return left->added();
                         if (left->removed() != right->removed())
                           return !left->removed();
                         if (left->removed())
                           return left->depth() > right->depth();
                         return left->depth() < right->depth();
                       });
      try {
        for (const auto &entry : work.entries) entry->prepare();
      } catch (...) {
        reject_flush(work);
        throw;
      }
      return work;
    }

    static void execute_flush(const flush_work &work, transaction &owner) {
      for (const auto &entry : work.entries) entry->flush(owner);
    }

    void accept_flush(const flush_work &work) {
      for (const auto &entry : work.entries) entry->accept();
      for (const auto &tracker : relation_trackers_) tracker->accept();
      for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        if (iterator->second->removed()) {
          relation_masks_.erase(iterator->first);
          iterator = entries_.erase(iterator);
        } else {
          ++iterator;
        }
      }
      for (auto iterator = relation_trackers_.begin();
           iterator != relation_trackers_.end();) {
        if (entries_.find((*iterator)->owner_key()) == entries_.end())
          iterator = relation_trackers_.erase(iterator);
        else
          ++iterator;
      }
    }

    void reject_flush(const flush_work &work) noexcept {
      for (const auto &tracker : relation_trackers_) tracker->reject();
      for (const auto &entry : work.entries) entry->reject();
    }

    template <typename Entity, typename Id>
    [[nodiscard]] std::optional<Entity> find_row_by_id(Id &&id) const {
      const repository<Entity> target(*connection_);
      if (active_transaction_ != nullptr)
        return target.find_by_id(std::forward<Id>(id), *active_transaction_);
      return target.find_by_id(std::forward<Id>(id));
    }

    template <typename Entity, typename Query>
    [[nodiscard]] std::vector<Entity> fetch_rows(Query &target) const {
      if (active_transaction_ != nullptr)
        return target.template fetch<Entity>(*active_transaction_);
      return target.template fetch<Entity>();
    }

    template <typename Entity>
    [[nodiscard]] std::uint64_t
    loaded_relation_mask(const std::shared_ptr<Entity> &entity) const {
      if (!entity) return 0;
      const auto found =
          relation_masks_.find(detail::identity_key_for_entity(*entity));
      return found == relation_masks_.end() ? 0 : found->second;
    }

    template <typename Entity>
    void hydrate_graph(
        const std::shared_ptr<Entity> &entity,
        model::entity_graph<std::remove_cv_t<std::remove_reference_t<Entity>>> requested) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      if constexpr (detail::has_entity_graph_v<entity_type>) {
        if (!entity)
          throw std::invalid_argument("ORM session cannot hydrate a null entity");
        const std::uint64_t unsupported =
            requested.mask() &
            ~model::entity_model<entity_type>::loadable_graph().mask();
        if (unsupported != 0)
          throw std::invalid_argument(
              "ORM entity graph contains a relation without fetch metadata");
        const auto key = detail::identity_key_for_entity(*entity);
        const std::uint64_t current = loaded_relation_mask(entity);
        const std::uint64_t missing = requested.mask() & ~current;
        if (missing == 0) return;

        graph_checkpoint checkpoint(*this);
        relation_masks_[key] = current | missing;
        const model::entity_graph<entity_type> missing_graph(missing);
        try {
          model::entity_model<entity_type>::load_graph(*this, *entity,
                                                        missing_graph);
          model::entity_model<entity_type>::for_each_relation(
              missing_graph, [&](const auto &relation) {
                register_relation_tracker(entity, relation);
              });
        } catch (...) {
          checkpoint.restore(*this);
          throw;
        }
      }
    }

    template <graph_lifecycle Operation, typename Entity, typename Tuple>
    void apply_graph_descriptors(const std::shared_ptr<Entity> &entity,
                                 Tuple &&relations) {
      std::apply(
          [&](const auto &...descriptor) {
            if constexpr (Operation == graph_lifecycle::remove)
              remove_graph(entity, descriptor...);
            else if constexpr (Operation == graph_lifecycle::refresh)
              refresh_graph(entity, descriptor...);
            else
              detach_graph(entity, descriptor...);
          },
          std::forward<Tuple>(relations));
    }

    template <graph_lifecycle Operation, typename Entity>
    void apply_single_lifecycle(const std::shared_ptr<Entity> &entity) {
      if constexpr (Operation == graph_lifecycle::remove)
        remove_at_depth(entity, 0);
      else if constexpr (Operation == graph_lifecycle::refresh)
        refresh_one(entity);
      else
        detach_one(entity);
    }

    template <graph_lifecycle Operation, typename Entity>
    void apply_model_graph_lifecycle(const std::shared_ptr<Entity> &entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      if constexpr (detail::has_entity_graph_v<entity_type>) {
        const std::uint64_t mask = loaded_relation_mask(entity);
        if (mask == 0) {
          apply_single_lifecycle<Operation>(entity);
          return;
        }
        const model::entity_graph<entity_type> graph(mask);
        if constexpr (Operation == graph_lifecycle::detach) {
          if (!entity)
            throw std::invalid_argument(
                "ORM session cannot cascade-detach a null entity");
          const auto managed_root = find_managed_entry(entity);
          if (!managed_root) return;
          std::vector<std::shared_ptr<detail::managed_entry_base>> detach_plan;
          model::entity_model<entity_type>::for_each_relation(
              graph, [&](const auto &relation) {
                collect_cascade_detach(*entity, relation, 0, detach_plan);
              });
          detach_plan.push_back(managed_root);
          std::unordered_set<const detail::managed_entry_base *> seen;
          for (const auto &entry : detach_plan) {
            if (seen.insert(entry.get()).second) detach_entry(entry);
          }
        } else {
          graph_checkpoint checkpoint(*this);
          try {
            if constexpr (Operation == graph_lifecycle::refresh) {
              refresh_one(entity);
              model::entity_model<entity_type>::for_each_relation(
                  graph, [&](const auto &relation) {
                    cascade_refresh(*entity, relation, 0);
                  });
              rebase_relation_trackers(entity.get());
            } else {
              if (!entity)
                throw std::invalid_argument(
                    "ORM session cannot cascade-remove a null entity");
              model::entity_model<entity_type>::for_each_relation(
                  graph, [&](const auto &relation) {
                    cascade_remove(entity, relation, 0);
                  });
              remove_at_depth(entity, 0);
            }
          } catch (...) {
            checkpoint.restore(*this);
            throw;
          }
        }
      } else {
        apply_graph_descriptors<Operation>(
            entity, model::entity_model<entity_type>::relations());
      }
    }

    void require_entity_capacity() const {
      if (entries_.size() >= max_entities_)
        throw status_error(ORM_STATUS_LIMIT_EXCEEDED,
                           "ORM session managed-entity limit exceeded");
    }

    template <typename Entity>
    [[nodiscard]]
        std::shared_ptr<detail::managed_entry<
            std::remove_cv_t<std::remove_reference_t<Entity>>>>
        find_managed_entry(const std::shared_ptr<Entity> &entity) const {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      static_assert(detail::has_model_v<entity_type>,
                    "ORM session lifecycle operations require an entity model");
      if (!entity) return {};
      return find_managed_entry_by_address(entity.get());
    }

    template <typename Entity>
    [[nodiscard]]
        std::shared_ptr<detail::managed_entry<
            std::remove_cv_t<std::remove_reference_t<Entity>>>>
        find_managed_entry_by_address(const Entity *entity) const {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      if (entity == nullptr) return {};
      for (const auto &item : entries_) {
        auto managed =
            std::dynamic_pointer_cast<detail::managed_entry<entity_type>>(item.second);
        if (managed && &managed->current == entity) return managed;
      }
      return {};
    }

    template <typename Entity>
    [[nodiscard]]
        std::shared_ptr<detail::managed_entry<
            std::remove_cv_t<std::remove_reference_t<Entity>>>>
    find_managed_entry_by_value(const Entity &entity) const {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      const auto found = entries_.find(detail::identity_key_for_entity(entity));
      if (found == entries_.end()) return {};
      auto managed =
          std::dynamic_pointer_cast<detail::managed_entry<entity_type>>(found->second);
      if (!managed) throw std::logic_error("ORM session identity-map type mismatch");
      return managed;
    }

    template <typename Entity>
    void detach_one(const std::shared_ptr<Entity> &entity) {
      if (!entity) throw std::invalid_argument("ORM session cannot detach a null entity");
      const auto managed = find_managed_entry(entity);
      if (!managed) return;
      detach_entry(managed);
    }

    template <typename Entity>
    void refresh_one(const std::shared_ptr<Entity> &entity) {
      if (!entity) throw std::invalid_argument("ORM session cannot refresh a null entity");
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      const auto managed = find_managed_entry(entity);
      if (!managed)
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM session cannot refresh a detached entity");
      if (managed->added() || managed->removed())
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM session cannot refresh an added or removed entity");

      auto row = find_row_by_id<entity_type>(
          detail::primary_key_id(*managed->original));
      if (!row.has_value())
        throw status_error(ORM_STATUS_DATASTORE_ERROR,
                           "ORM session cannot refresh a missing entity");

      entity_type refreshed = managed->current;
      detail::copy_modeled_fields(refreshed, *row);
      detail::invoke_lifecycle(model::lifecycle_event::post_load, refreshed);
      if (!(detail::identity_key_for_entity(refreshed) == managed->identity))
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM lifecycle callback cannot change the primary key");
      managed->refresh(std::move(refreshed));
    }

    void detach_entry(const std::shared_ptr<detail::managed_entry_base> &managed) {
      const void *address = managed->object_address();
      relation_trackers_.erase(
          std::remove_if(relation_trackers_.begin(), relation_trackers_.end(),
                         [address](const auto &tracker) {
                           return tracker->owner_address() == address;
                         }),
          relation_trackers_.end());
      for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
        if (iterator->second == managed) {
          relation_masks_.erase(iterator->first);
          entries_.erase(iterator);
          return;
        }
      }
    }

    static void require_cascade_depth(std::size_t depth) {
      if (depth >= max_cascade_depth)
        throw status_error(ORM_STATUS_LIMIT_EXCEEDED,
                           "ORM relation cascade depth limit exceeded");
    }

    template <typename Entity>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    persist_at_depth(Entity entity, std::size_t relation_depth) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      static_assert(detail::has_model_v<entity_type>,
                    "ORM session persist<T> requires an entity model");
      static_assert(std::is_default_constructible_v<entity_type>,
                    "ORM session persist<T> requires a default-constructible entity type");
      using entry_type = detail::managed_entry<entity_type>;

      const auto key = detail::identity_key_for_entity(entity);
      const auto found = entries_.find(key);
      if (found != entries_.end()) {
        if (found->second->removed())
          throw status_error(ORM_STATUS_INVALID_STATE,
                             "ORM session cannot persist an entity pending removal");
        auto typed = std::dynamic_pointer_cast<entry_type>(found->second);
        if (!typed) throw std::logic_error("ORM session identity-map type mismatch");
        return std::shared_ptr<entity_type>(typed, &typed->current);
      }

      require_entity_capacity();
      auto managed = std::make_shared<entry_type>(std::move(entity), detail::managed_state::added,
                                                  relation_depth);
      managed->connection = connection_;
      entries_.emplace(key, managed);
      return std::shared_ptr<entity_type>(managed, &managed->current);
    }

    template <typename Entity>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    merge_at_depth(const Entity &entity, std::size_t relation_depth) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      static_assert(detail::has_model_v<entity_type>,
                    "ORM session merge<T> requires an entity model");
      const auto key = detail::identity_key_for_entity(entity);
      const auto found = entries_.find(key);
      if (found != entries_.end()) {
        auto managed =
            std::dynamic_pointer_cast<detail::managed_entry<entity_type>>(found->second);
        if (!managed) throw std::logic_error("ORM session identity-map type mismatch");
        if (managed->removed())
          throw status_error(ORM_STATUS_INVALID_STATE,
                             "ORM session cannot merge an entity pending removal");
        managed->current = entity;
        managed->relation_depth = std::max(managed->relation_depth, relation_depth);
        return std::shared_ptr<entity_type>(managed, &managed->current);
      }

      auto row = find_row_by_id<entity_type>(detail::primary_key_id(entity));
      if (!row.has_value()) return persist_at_depth(entity, relation_depth);

      auto managed = adopt_loaded(std::move(*row));
      auto entry = find_managed_entry(managed);
      if (!entry) throw std::logic_error("ORM session failed to manage merged entity");
      entry->current = entity;
      entry->relation_depth = std::max(entry->relation_depth, relation_depth);
      return managed;
    }

    template <typename Entity>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    load_existing_for_value(const Entity &entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      if (auto managed = find_managed_entry_by_value(entity))
        return std::shared_ptr<entity_type>(managed, &managed->current);

      auto row = find_row_by_id<entity_type>(detail::primary_key_id(entity));
      if (!row.has_value()) return {};
      return adopt_loaded(std::move(*row));
    }

    template <typename Entity>
    void remove_at_depth(const std::shared_ptr<Entity> &entity, std::size_t relation_depth) {
      if (!entity) throw std::invalid_argument("ORM session cannot remove a null entity");
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      const auto key = detail::identity_key_for_entity(*entity);
      auto found = entries_.find(key);
      if (found == entries_.end()) {
        auto managed = adopt_loaded(*entity);
        found = entries_.find(key);
        if (found == entries_.end())
          throw std::logic_error("ORM session failed to manage entity for removal");
        (void)managed;
      }
      if (found->second->added()) {
        relation_masks_.erase(found->first);
        entries_.erase(found);
        return;
      }
      auto typed = std::dynamic_pointer_cast<detail::managed_entry<entity_type>>(found->second);
      if (!typed) throw std::logic_error("ORM session identity-map type mismatch");
      typed->mark_removed(relation_depth);
    }

    template <typename Owner, typename Child, typename... Nested, typename Visitor>
    static void for_each_relation_child(
        Owner &owner, const relation::has_one<Owner, Child, Nested...> &descriptor,
        Visitor &&visitor) {
      visitor(owner.*descriptor.member);
    }

    template <typename Owner, typename Child, typename... Nested, typename Visitor>
    static void for_each_relation_child(
        Owner &owner, const relation::has_many<Owner, Child, Nested...> &descriptor,
        Visitor &&visitor) {
      for (auto &child : owner.*descriptor.member) visitor(child);
    }

    template <typename Owner, typename Child, typename... Nested, typename Visitor>
    static void for_each_relation_child(
        Owner &owner, const relation::optional_one<Owner, Child, Nested...> &descriptor,
        Visitor &&visitor) {
      auto &child = owner.*descriptor.member;
      if (child.has_value()) visitor(*child);
    }

    template <typename Owner, typename Child, typename... Nested, typename Visitor>
    static void for_each_relation_child(
        Owner &owner, const relation::shared_one<Owner, Child, Nested...> &descriptor,
        Visitor &&visitor) {
      auto &child = owner.*descriptor.member;
      if (child) visitor(*child);
    }

    template <typename Owner, typename Child, typename... Nested, typename Visitor>
    static void for_each_relation_child(
        Owner &owner, const relation::shared_many<Owner, Child, Nested...> &descriptor,
        Visitor &&visitor) {
      for (auto &child : owner.*descriptor.member) {
        if (!child)
          throw std::invalid_argument("ORM relation collection contains a null entity");
        visitor(*child);
      }
    }

    template <typename Owner, typename Descriptor>
    void cascade_merge(Owner &owner, const Descriptor &descriptor, std::size_t depth) {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::merge)) return;
      require_cascade_depth(depth);
      for_each_relation_child(owner, descriptor, [&](auto &value) {
        if (descriptor.bind_foreign_key) descriptor.bind_foreign_key(owner, value);
        auto child = merge_at_depth(value, depth + 1);
        std::apply(
            [&](const auto &...nested) { (cascade_merge(*child, nested, depth + 1), ...); },
            descriptor.nested);
      });
    }

    template <typename Owner, typename Descriptor>
    void cascade_refresh(Owner &owner, const Descriptor &descriptor, std::size_t depth) {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::refresh)) return;
      require_cascade_depth(depth);
      for_each_relation_child(owner, descriptor, [&](auto &value) {
        auto child = load_existing_for_value(value);
        if (!child)
          throw status_error(ORM_STATUS_DATASTORE_ERROR,
                             "ORM session cannot cascade-refresh a missing entity");
        refresh_one(child);
        detail::copy_modeled_fields(value, *child);
        std::apply(
            [&](const auto &...nested) { (cascade_refresh(value, nested, depth + 1), ...); },
            descriptor.nested);
      });
    }

    template <typename Owner, typename Descriptor>
    void collect_cascade_detach(
        Owner &owner, const Descriptor &descriptor, std::size_t depth,
        std::vector<std::shared_ptr<detail::managed_entry_base>> &detach_plan) const {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::detach)) return;
      require_cascade_depth(depth);
      for_each_relation_child(owner, descriptor, [&](auto &value) {
        std::apply(
            [&](const auto &...nested) {
              (collect_cascade_detach(value, nested, depth + 1, detach_plan), ...);
            },
            descriptor.nested);
        auto child = find_managed_entry_by_address(&value);
        if (!child) child = find_managed_entry_by_value(value);
        if (child) detach_plan.push_back(std::move(child));
      });
    }

    template <typename Owner, typename Child, typename... Nested>
    void cascade_persist(const std::shared_ptr<Owner> &owner,
                         const relation::has_one<Owner, Child, Nested...> &descriptor,
                         std::size_t depth) {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::persist)) return;
      require_cascade_depth(depth);
      auto &child_value = (*owner).*descriptor.member;
      if (descriptor.bind_foreign_key) descriptor.bind_foreign_key(*owner, child_value);
      auto child = persist_at_depth(child_value, depth + 1);
      std::apply(
          [&](const auto &...nested) { (cascade_persist(child, nested, depth + 1), ...); },
          descriptor.nested);
    }

    template <typename Owner, typename Child, typename... Nested>
    void cascade_persist(const std::shared_ptr<Owner> &owner,
                         const relation::has_many<Owner, Child, Nested...> &descriptor,
                         std::size_t depth) {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::persist)) return;
      require_cascade_depth(depth);
      for (auto &value : (*owner).*descriptor.member) {
        if (descriptor.bind_foreign_key) descriptor.bind_foreign_key(*owner, value);
        auto child = persist_at_depth(value, depth + 1);
        std::apply(
            [&](const auto &...nested) { (cascade_persist(child, nested, depth + 1), ...); },
            descriptor.nested);
      }
    }

    template <typename Owner, typename Child, typename... Nested>
    void cascade_persist(const std::shared_ptr<Owner> &owner,
                         const relation::optional_one<Owner, Child, Nested...> &descriptor,
                         std::size_t depth) {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::persist)) return;
      auto &value = (*owner).*descriptor.member;
      if (!value.has_value()) return;
      require_cascade_depth(depth);
      if (descriptor.bind_foreign_key) descriptor.bind_foreign_key(*owner, *value);
      auto child = persist_at_depth(*value, depth + 1);
      std::apply(
          [&](const auto &...nested) { (cascade_persist(child, nested, depth + 1), ...); },
          descriptor.nested);
    }

    template <typename Owner, typename Child, typename... Nested>
    void cascade_persist(const std::shared_ptr<Owner> &owner,
                         const relation::shared_one<Owner, Child, Nested...> &descriptor,
                         std::size_t depth) {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::persist)) return;
      auto &value = (*owner).*descriptor.member;
      if (!value) return;
      require_cascade_depth(depth);
      if (descriptor.bind_foreign_key) descriptor.bind_foreign_key(*owner, *value);
      auto child = persist_at_depth(*value, depth + 1);
      std::apply(
          [&](const auto &...nested) { (cascade_persist(child, nested, depth + 1), ...); },
          descriptor.nested);
    }

    template <typename Owner, typename Child, typename... Nested>
    void cascade_persist(const std::shared_ptr<Owner> &owner,
                         const relation::shared_many<Owner, Child, Nested...> &descriptor,
                         std::size_t depth) {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::persist)) return;
      require_cascade_depth(depth);
      for (const auto &value : (*owner).*descriptor.member) {
        if (!value)
          throw std::invalid_argument("ORM relation collection contains a null entity");
        if (descriptor.bind_foreign_key) descriptor.bind_foreign_key(*owner, *value);
        auto child = persist_at_depth(*value, depth + 1);
        std::apply(
            [&](const auto &...nested) { (cascade_persist(child, nested, depth + 1), ...); },
            descriptor.nested);
      }
    }

    template <typename Owner, typename Child, typename... Nested>
    void cascade_remove(const std::shared_ptr<Owner> &owner,
                        const relation::has_one<Owner, Child, Nested...> &descriptor,
                        std::size_t depth) {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::remove)) return;
      require_cascade_depth(depth);
      auto child = adopt_loaded((*owner).*descriptor.member);
      std::apply(
          [&](const auto &...nested) { (cascade_remove(child, nested, depth + 1), ...); },
          descriptor.nested);
      remove_at_depth(child, depth + 1);
    }

    template <typename Owner, typename Child, typename... Nested>
    void cascade_remove(const std::shared_ptr<Owner> &owner,
                        const relation::has_many<Owner, Child, Nested...> &descriptor,
                        std::size_t depth) {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::remove)) return;
      require_cascade_depth(depth);
      for (const auto &value : (*owner).*descriptor.member) {
        auto child = adopt_loaded(value);
        std::apply(
            [&](const auto &...nested) { (cascade_remove(child, nested, depth + 1), ...); },
            descriptor.nested);
        remove_at_depth(child, depth + 1);
      }
    }

    template <typename Owner, typename Child, typename... Nested>
    void cascade_remove(const std::shared_ptr<Owner> &owner,
                        const relation::optional_one<Owner, Child, Nested...> &descriptor,
                        std::size_t depth) {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::remove)) return;
      const auto &value = (*owner).*descriptor.member;
      if (!value.has_value()) return;
      require_cascade_depth(depth);
      auto child = adopt_loaded(*value);
      std::apply(
          [&](const auto &...nested) { (cascade_remove(child, nested, depth + 1), ...); },
          descriptor.nested);
      remove_at_depth(child, depth + 1);
    }

    template <typename Owner, typename Child, typename... Nested>
    void cascade_remove(const std::shared_ptr<Owner> &owner,
                        const relation::shared_one<Owner, Child, Nested...> &descriptor,
                        std::size_t depth) {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::remove)) return;
      const auto &value = (*owner).*descriptor.member;
      if (!value) return;
      require_cascade_depth(depth);
      auto child = adopt_loaded(*value);
      std::apply(
          [&](const auto &...nested) { (cascade_remove(child, nested, depth + 1), ...); },
          descriptor.nested);
      remove_at_depth(child, depth + 1);
    }

    template <typename Owner, typename Child, typename... Nested>
    void cascade_remove(const std::shared_ptr<Owner> &owner,
                        const relation::shared_many<Owner, Child, Nested...> &descriptor,
                        std::size_t depth) {
      if (!relation::includes(descriptor.policy, relation::cascade_policy::remove)) return;
      require_cascade_depth(depth);
      for (const auto &value : (*owner).*descriptor.member) {
        if (!value)
          throw std::invalid_argument("ORM relation collection contains a null entity");
        auto child = adopt_loaded(*value);
        std::apply(
            [&](const auto &...nested) { (cascade_remove(child, nested, depth + 1), ...); },
            descriptor.nested);
        remove_at_depth(child, depth + 1);
      }
    }

    template <typename Entity>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    adopt_loaded(Entity &&entity) {
      using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
      using entry_type = detail::managed_entry<entity_type>;
      entity_type candidate(std::forward<Entity>(entity));
      const auto key = detail::identity_key_for_entity(candidate);
      const auto found = entries_.find(key);
      if (found != entries_.end()) {
        auto typed = std::dynamic_pointer_cast<entry_type>(found->second);
        if (!typed) throw std::logic_error("ORM session identity-map type mismatch");
        return std::shared_ptr<entity_type>(typed, &typed->current);
      }

      require_entity_capacity();
      detail::invoke_lifecycle(model::lifecycle_event::post_load, candidate);
      if (!(detail::identity_key_for_entity(candidate) == key))
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM lifecycle callback cannot change the primary key");
      auto managed = std::make_shared<entry_type>(std::move(candidate));
      managed->connection = connection_;
      entries_.emplace(key, managed);
      return std::shared_ptr<entity_type>(managed, &managed->current);
    }

    const connection *connection_ = nullptr;
    std::shared_ptr<detail::session_lifetime> lifetime_;
    std::size_t max_entities_ = 0;
    entry_map entries_;
    std::vector<std::shared_ptr<relation_tracker_base>> relation_trackers_;
    relation_mask_map relation_masks_;
    transaction *active_transaction_ = nullptr;
    bool rollback_only_ = false;
  };

  using entity_manager = session;

  template <typename Entity, typename Id> class lazy_one final {
  public:
    using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
    using id_type = std::remove_cv_t<std::remove_reference_t<Id>>;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

    [[nodiscard]] std::shared_ptr<entity_type> get() const {
      auto lifetime = lifetime_.lock();
      if (!lifetime || lifetime->owner == nullptr)
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM lazy relation outlived its session");
      if (!loaded_) {
        value_ = foreign_key_.empty()
                     ? lifetime->owner->template load<entity_type>(id_)
                     : lifetime->owner->template find_one<entity_type>(foreign_key_, id_);
        loaded_ = true;
      }
      return value_;
    }

    [[nodiscard]] bool has_value() const { return get() != nullptr; }

    entity_type &value() const {
      auto loaded_value = get();
      if (!loaded_value)
        throw status_error(ORM_STATUS_DATASTORE_ERROR,
                           "ORM lazy relation target was not found");
      return *loaded_value;
    }

    entity_type *operator->() { return &value(); }
    const entity_type *operator->() const { return &value(); }
    entity_type &operator*() { return value(); }
    const entity_type &operator*() const { return value(); }

  private:
    friend class session;

    lazy_one(const std::shared_ptr<detail::session_lifetime> &lifetime, id_type id)
        : lifetime_(lifetime), id_(std::move(id)) {}

    lazy_one(const std::shared_ptr<detail::session_lifetime> &lifetime,
             std::string foreign_key, id_type id)
        : lifetime_(lifetime), id_(std::move(id)), foreign_key_(std::move(foreign_key)) {}

    std::weak_ptr<detail::session_lifetime> lifetime_;
    id_type id_;
    std::string foreign_key_;
    mutable std::shared_ptr<entity_type> value_;
    mutable bool loaded_ = false;
  };

  template <typename Entity, typename ForeignKey> class lazy_many final {
  public:
    using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
    using foreign_key_type = std::remove_cv_t<std::remove_reference_t<ForeignKey>>;
    using collection_type = std::vector<std::shared_ptr<entity_type>>;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

    [[nodiscard]] const collection_type &get() const {
      auto lifetime = lifetime_.lock();
      if (!lifetime || lifetime->owner == nullptr)
        throw status_error(ORM_STATUS_INVALID_STATE,
                           "ORM lazy relation outlived its session");
      if (!loaded_) {
        values_ = lifetime->owner->template find_many<entity_type>(foreign_key_, value_);
        loaded_ = true;
      }
      return values_;
    }

    [[nodiscard]] bool empty() const { return get().empty(); }
    [[nodiscard]] std::size_t size() const { return get().size(); }
    [[nodiscard]] const std::shared_ptr<entity_type> &at(std::size_t index) const {
      return get().at(index);
    }

  private:
    friend class session;

    lazy_many(const std::shared_ptr<detail::session_lifetime> &lifetime,
              std::string foreign_key, foreign_key_type value)
        : lifetime_(lifetime), foreign_key_(std::move(foreign_key)),
          value_(std::move(value)) {}

    std::weak_ptr<detail::session_lifetime> lifetime_;
    std::string foreign_key_;
    foreign_key_type value_;
    mutable collection_type values_;
    mutable bool loaded_ = false;
  };

  template <typename Entity, typename Id>
  lazy_one<std::remove_cv_t<std::remove_reference_t<Entity>>, std::decay_t<Id>>
  session::defer(Id &&id) {
    using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
    using id_type = std::decay_t<Id>;
    return lazy_one<entity_type, id_type>(lifetime_, std::forward<Id>(id));
  }

  template <typename Entity, typename ForeignKey>
  lazy_many<std::remove_cv_t<std::remove_reference_t<Entity>>, std::decay_t<ForeignKey>>
  session::defer_many(std::string_view foreign_key, ForeignKey &&value) {
    if (foreign_key.empty())
      throw std::invalid_argument("ORM lazy relation foreign-key column is empty");
    using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
    using key_type = std::decay_t<ForeignKey>;
    return lazy_many<entity_type, key_type>(lifetime_, std::string(foreign_key),
                                            std::forward<ForeignKey>(value));
  }

  template <typename Entity, typename ForeignKey>
  lazy_one<std::remove_cv_t<std::remove_reference_t<Entity>>, std::decay_t<ForeignKey>>
  session::defer_one(std::string_view foreign_key, ForeignKey &&value) {
    if (foreign_key.empty())
      throw std::invalid_argument("ORM lazy relation foreign-key column is empty");
    using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;
    using key_type = std::decay_t<ForeignKey>;
    return lazy_one<entity_type, key_type>(lifetime_, std::string(foreign_key),
                                           std::forward<ForeignKey>(value));
  }

} // namespace orm

#endif
