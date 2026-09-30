#ifndef ORM_FLOW_HPP
#define ORM_FLOW_HPP

#include "orm.h"
#include <cflow/function_projection.h>
#include <cflow/lower.h>
#include <memory>
#include <stdexcept>
#include <utility>

namespace orm {
template <typename Row> class publisher;

class flow_error final : public std::runtime_error {
public:
  explicit flow_error(const char *message)
      : std::runtime_error(message ? message : "CFlow operation failed") {}
};

namespace detail {
// The address remains stable after moves: CFlow borrows both graphs through close.
struct flow_state final {
  orm_query_t *query = nullptr;
  cflow_publisher source{};
  cflow_graph surface{};
  cflow_graph normalized{};
  cflow_subscription subscription{};
  ~flow_state() {
    cflow_subscription_close(&subscription);
    if (cflow_publisher_valid(&source)) cflow_publisher_destroy(&source);
    cflow_graph_destroy(&normalized);
    cflow_graph_destroy(&surface);
    orm_query_destroy(query);
  }
};
} // namespace detail

// Single owner. Scheduler, descriptors and subscriber state outlive close().
// close/destruction must occur outside subscriber callbacks and active pumps.
class subscription final {
public:
  subscription(const subscription &) = delete;
  subscription &operator=(const subscription &) = delete;
  subscription(subscription &&) noexcept = default;
  subscription &operator=(subscription &&) noexcept = default;
  [[nodiscard]] cflow_status_result request(std::size_t count) {
    return cflow_subscription_request_result(native(), count);
  }
  void cancel() noexcept { if (state_) cflow_subscription_cancel(native()); }
  void close() noexcept { state_.reset(); }
  [[nodiscard]] bool done() const noexcept {
    return !state_ || cflow_subscription_is_done(&state_->subscription);
  }
  [[nodiscard]] cflow_status status() const noexcept {
    return state_ ? cflow_subscription_status(&state_->subscription) : CFLOW_STATUS_CLOSED;
  }
  // Borrowed diagnostic, invalidated by close or move assignment.
  [[nodiscard]] const char *error() const noexcept {
    return state_ ? cflow_subscription_error(&state_->subscription) : nullptr;
  }
private:
  friend class flow;
  explicit subscription(std::unique_ptr<detail::flow_state> state) noexcept
      : state_(std::move(state)) {}
  cflow_subscription *native() noexcept {
    return state_ ? &state_->subscription : nullptr;
  }
  std::unique_ptr<detail::flow_state> state_;
};

// One-shot result pipeline. Operators execute in CFlow; filter is client-side.
class flow final {
public:
  flow(const flow &) = delete;
  flow &operator=(const flow &) = delete;
  flow(flow &&) noexcept = default;
  flow &operator=(flow &&) noexcept = default;
  flow &filter(cflow_filter_callable predicate) {
    auto &s = state();
    check(cflow_graph_add(&s.surface, CFLOW_OP_FILTER, predicate.fn, nullptr));
    return *this;
  }
  flow &map(cflow_map_callable transform) {
    auto &s = state();
    check(cflow_graph_add(&s.surface, CFLOW_OP_MAP, transform.fn, nullptr));
    return *this;
  }
  flow &take(std::size_t count) {
    auto &s = state();
    check(cflow_graph_take(&s.surface, count));
    return *this;
  }
  flow &filter(const cflow_function_typed_adapter_projection &predicate) {
    check(cflow_graph_add_function_typed_filter_projection(&state().surface, &predicate));
    return *this;
  }
  flow &map(const cflow_function_typed_adapter_projection &transform) {
    check(cflow_graph_add_function_typed_adapter_projection(&state().surface, &transform));
    return *this;
  }
  [[nodiscard]] subscription subscribe(cflow_scheduler &scheduler,
                                        const cflow_subscriber &subscriber) {
    auto &s = state();
    if (!cflow_scheduler_valid(&scheduler) || !cflow_subscriber_valid(&subscriber))
      throw flow_error("subscription requires a valid scheduler and subscriber");
    cflow_graph_destroy(&s.normalized);
    s.normalized = {};
    s.normalized.root = CMETA_INVALID_ID;
    if (!cflow_graph_normalize(&s.normalized, &s.surface))
      throw flow_error(s.normalized.error);
    const auto result = cflow_subscribe_with_options(&s.subscription,
        &s.normalized, &s.source, &scheduler, &subscriber, nullptr);
    if (!cflow_status_result_is_ok(result))
      throw flow_error(cflow_status_result_message(result));
    return subscription(std::move(state_));
  }
private:
  template <typename Row> friend class publisher;
  flow(orm_query_t *&query, cflow_publisher &source)
      : state_(std::make_unique<detail::flow_state>()) {
    if (!cflow_publisher_valid(&source)) throw flow_error("publisher is empty");
    cflow_graph_init(&state_->surface, cflow_publisher_output_type(&source));
    check(state_->surface.error == nullptr);
    state_->query = std::exchange(query, nullptr);
    state_->source = std::exchange(source, cflow_publisher{});
  }
  detail::flow_state &state() {
    if (!state_) throw flow_error("pipeline has already been moved or subscribed");
    return *state_;
  }
  void check(bool result) {
    if (!result || state_->surface.error)
      throw flow_error(state_->surface.error);
  }
  std::unique_ptr<detail::flow_state> state_;
};
} // namespace orm
#endif
