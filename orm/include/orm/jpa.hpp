#ifndef ORM_JPA_HPP
#define ORM_JPA_HPP

/*
 * Thin JPA-style facade over orm::repository and orm::session.
 *
 * This header intentionally preserves existing repository/session semantics and
 * re-exposes them with JPA-style naming.
 */

#include "orm.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace orm {
namespace jpa {

  template <typename T>
  class specification final {
  public:
    using entity_type = std::remove_cv_t<std::remove_reference_t<T>>;
    using query_type = orm::select_query;
    using applier_type = std::function<void(query_type &)>;

    specification() = default;

    template <typename Applier,
              typename = std::enable_if_t<
                  !std::is_same_v<std::decay_t<Applier>, specification<T>>>>
    explicit specification(Applier &&applier)
        : applier_(std::forward<Applier>(applier)) {}

    [[nodiscard]] bool empty() const noexcept { return !applier_.has_value(); }

    void apply(query_type &query) const {
      if (applier_) (*applier_)(query);
    }

  private:
    std::optional<applier_type> applier_;
  };

  template <typename Entity>
  class repository final {
  public:
    using entity_type = std::remove_cv_t<std::remove_reference_t<Entity>>;

    explicit repository(const orm::connection &owner) noexcept
        : connection_(&owner) {}

    [[nodiscard]] std::vector<entity_type> findAll() const {
      return native().find_all();
    }

    [[nodiscard]] std::vector<entity_type> findAll(
        const specification<entity_type> &query_spec) const {
      auto target = connection_->template select<entity_type>();
      query_spec.apply(target);
      return target.fetch<entity_type>();
    }

    [[nodiscard]] std::optional<entity_type> findOne(
        const specification<entity_type> &query_spec) const {
      auto target = connection_->template select<entity_type>();
      query_spec.apply(target);
      auto output = target.fetch();
      const auto count = output.rows();
      if (count == 0) return std::nullopt;
      if (count != 1)
        throw orm::status_error(ORM_STATUS_INVALID_STATE,
                                "JPA repository.findOne(spec) expects exactly one row");
      return output.row<entity_type>(0);
    }

    template <typename Id>
    [[nodiscard]] std::optional<entity_type> findById(Id &&id) const {
      return native().find_by_id(std::forward<Id>(id));
    }

    template <typename Id>
    [[nodiscard]] std::optional<entity_type> getById(Id &&id) const {
      return findById(std::forward<Id>(id));
    }

    template <typename Id>
    [[nodiscard]] std::vector<entity_type> findAllById(std::vector<Id> ids) const {
      std::vector<entity_type> output;
      output.reserve(ids.size());
      for (auto &id : ids) {
        auto row = native().find_by_id(std::move(id));
        if (row.has_value()) output.push_back(std::move(row.value()));
      }
      return output;
    }

    [[nodiscard]] std::uint64_t count() const {
      auto target = connection_->template select<entity_type>();
      return target.fetch().rows();
    }

    [[nodiscard]] std::uint64_t count(const specification<entity_type> &query_spec) const {
      auto target = connection_->template select<entity_type>();
      query_spec.apply(target);
      return target.fetch().rows();
    }

    template <typename Id> [[nodiscard]] bool existsById(Id &&id) const {
      return native().find_by_id(std::forward<Id>(id)).has_value();
    }

    [[nodiscard]] bool exists(const specification<entity_type> &query_spec) const {
      return count(query_spec) > 0;
    }

    [[nodiscard]] result upsert(entity_type entity) {
      const auto entity_id = detail::primary_key_id(entity);
      if (native().find_by_id(entity_id).has_value())
        return native().update(entity);
      return native().insert(entity);
    }

    [[nodiscard]] result save(entity_type entity) {
      return upsert(std::move(entity));
    }

    [[nodiscard]] std::vector<result> saveAll(std::vector<entity_type> entities) {
      std::vector<result> outputs;
      outputs.reserve(entities.size());
      for (auto &entity : entities)
        outputs.push_back(save(std::move(entity)));
      return outputs;
    }

    [[nodiscard]] result saveAndFlush(entity_type entity) { return save(std::move(entity)); }

    [[nodiscard]] std::vector<result> saveAllAndFlush(std::vector<entity_type> entities) {
      return saveAll(std::move(entities));
    }

    template <typename Id>
    [[nodiscard]] result deleteById(Id &&id) const {
      return native().delete_by_id(std::forward<Id>(id));
    }

    template <typename Id>
    [[nodiscard]] std::vector<result> deleteAllById(std::vector<Id> ids) const {
      std::vector<result> outputs;
      outputs.reserve(ids.size());
      for (auto &id : ids) outputs.push_back(deleteById(std::move(id)));
      return outputs;
    }

    void deleteAll(const std::vector<entity_type> &entities) const {
      for (const auto &entity : entities)
        (void)delete_(entity);
    }

    [[nodiscard]] std::vector<result> deleteAll() const {
      const auto rows = findAll();
      std::vector<result> outputs;
      outputs.reserve(rows.size());
      for (auto &row : rows)
        outputs.push_back(delete_(row));
      return outputs;
    }

    [[nodiscard]] result delete_(const entity_type &entity) const {
      return delete_by_model(entity);
    }

    void delete_(const specification<entity_type> &query_spec) const {
      auto rows = findAll(query_spec);
      for (auto &entity : rows) {
        (void)delete_(entity);
      }
    }

  private:
    [[nodiscard]] result delete_by_model(const entity_type &entity) const {
      const auto primary_keys = model::get_primary_keys<entity_type>();
      orm::query target = connection_->delete_from(model::get_name<entity_type>());
      std::size_t matched = 0;
      detail::for_each_flat_field(entity, [&](std::string_view name, const auto &field) {
        for (const auto key : primary_keys) {
          if (name == key) {
            target.where(key, comparison::equal, detail::model_value(field));
            ++matched;
            return;
          }
        }
      });
      if (matched != primary_keys.size())
        throw std::invalid_argument(
            "JPA repository.delete(entity) cannot resolve all modeled primary-key fields");
      return target.execute();
    }

    [[nodiscard]] orm::repository<entity_type> native() const {
      return orm::repository<entity_type>(*connection_);
    }

    const orm::connection *connection_ = nullptr;
  };

  class entity_manager final {
  public:
    explicit entity_manager(const orm::connection &owner)
        : delegate_(owner), connection_(&owner) {}

    template <typename Entity, typename Id>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    find(Id &&id) {
      return delegate_.template find<Entity>(std::forward<Id>(id));
    }

    template <typename Entity, typename Id>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    load(Id &&id) {
      return delegate_.template load<Entity>(std::forward<Id>(id));
    }

    template <typename Entity>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    persist(Entity entity) {
      return delegate_.template persist<Entity>(std::move(entity));
    }

    template <typename Entity, typename Id>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>>
    getReference(Id &&id) {
      return load<Entity>(std::forward<Id>(id));
    }

    template <typename Entity>
    [[nodiscard]] std::shared_ptr<std::remove_cv_t<std::remove_reference_t<Entity>>> merge(
        const Entity &entity) {
      return delegate_.template merge<Entity>(entity);
    }

    template <typename Entity>
    void remove(const std::shared_ptr<Entity> &entity) {
      delegate_.template remove<Entity>(entity);
    }

    template <typename Entity, typename Id>
    void remove(Id &&id) {
      orm::repository<std::remove_cv_t<std::remove_reference_t<Entity>>> target(*connection_);
      (void)target.delete_by_id(std::forward<Id>(id));
    }

    template <typename Entity>
    void detach(const std::shared_ptr<Entity> &entity) {
      delegate_.template detach<Entity>(entity);
    }

    template <typename Entity>
    void refresh(const std::shared_ptr<Entity> &entity) {
      delegate_.template refresh<Entity>(entity);
    }

    template <typename Entity>
    bool contains(const std::shared_ptr<Entity> &entity) const {
      return delegate_.template contains<Entity>(entity);
    }

    template <typename Entity>
    orm::entity_state state(const std::shared_ptr<Entity> &entity) const {
      return delegate_.template state<Entity>(entity);
    }

    void flush() { (void)delegate_.flush(); }

    void discard() { delegate_.discard(); }

    void clear() { delegate_.clear(); }

    [[nodiscard]] bool dirty() const { return delegate_.dirty(); }

    [[nodiscard]] bool transaction_active() const noexcept {
      return delegate_.transaction_active();
    }

    template <typename Operation>
    auto transactional(
        Operation &&operation,
        orm::transaction_propagation propagation = orm::transaction_propagation::required,
        orm::isolation_level isolation = orm::isolation_level::serializable) {
      return delegate_.transactional(
          [&](orm::session &) {
            return std::invoke(std::forward<Operation>(operation), *this);
          },
          propagation, isolation);
    }

    [[nodiscard]] orm::session &unwrap() noexcept { return delegate_; }
    [[nodiscard]] const orm::session &unwrap() const noexcept { return delegate_; }

  private:
    const orm::connection *connection_ = nullptr;
    orm::session delegate_;
  };

} // namespace jpa
} // namespace orm

#endif  // ORM_JPA_HPP
