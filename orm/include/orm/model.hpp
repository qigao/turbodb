#ifndef ORM_MODEL_HPP
#define ORM_MODEL_HPP

/*
 * Canonical C++17 mapping contract for ORM entities.
 *
 * A model is discovered through entity_model_func(T const&) by ADL so mapped
 * structs can remain ordinary value types in any namespace. Schema-generated
 * headers and the ORM_MODEL_* macros both produce this single contract.
 */
#include "dbs/postgres/model_detail.hpp"

#include <cstdint>

namespace orm::model {

enum class lifecycle_event : std::uint8_t {
  pre_persist,
  post_persist,
  pre_update,
  post_update,
  pre_remove,
  post_remove,
  post_load
};

enum class inheritance_strategy : std::uint8_t {
  single_table,
  joined,
  table_per_class
};

template <typename Entity> struct discriminator_case final {
  using entity_type = Entity;
  std::string_view value;
};

template <typename Entity> class entity_graph final {
public:
  constexpr entity_graph() noexcept = default;
  explicit constexpr entity_graph(std::uint64_t mask) noexcept : mask_(mask) {}

  [[nodiscard]] constexpr std::uint64_t mask() const noexcept { return mask_; }

  [[nodiscard]] constexpr bool contains(entity_graph relation) const noexcept {
    return (mask_ & relation.mask_) == relation.mask_;
  }

  friend constexpr entity_graph operator|(entity_graph left,
                                          entity_graph right) noexcept {
    return entity_graph(left.mask_ | right.mask_);
  }

private:
  std::uint64_t mask_ = 0;
};

} // namespace orm::model

#endif  // ORM_MODEL_HPP
