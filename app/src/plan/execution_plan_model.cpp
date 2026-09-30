#include "plan/execution_plan_model.h"

#include <limits>
#include <utility>

namespace turbodb::app {

namespace {

std::string ViewToString(orm_string_view_t view) {
  if (view.data == nullptr || view.len == 0u) return {};
  return std::string(static_cast<const char*>(view.data), view.len);
}

bool AddBytes(ExecutionPlanSnapshot& snapshot, std::uint64_t bytes,
              const ExecutionPlanCopyLimits& limits, std::string* error) {
  if (snapshot.payload_bytes > limits.max_bytes ||
      bytes > limits.max_bytes - snapshot.payload_bytes) {
    if (error != nullptr) *error = "app execution-plan snapshot exceeds byte limit";
    return false;
  }
  snapshot.payload_bytes += bytes;
  return true;
}

bool CopyView(orm_string_view_t view, ExecutionPlanSnapshot& snapshot,
              const ExecutionPlanCopyLimits& limits, std::string* out,
              std::string* error) {
  if (view.len != 0u && view.data == nullptr) {
    if (error != nullptr) *error = "execution-plan API returned an invalid string view";
    return false;
  }
  if (!AddBytes(snapshot, view.len, limits, error)) return false;
  *out = ViewToString(view);
  return true;
}

}  // namespace

bool CopyExecutionPlan(orm_execution_plan_t* plan, std::uint64_t request_id,
                       std::uint64_t elapsed_microseconds,
                       const ExecutionPlanCopyLimits& limits,
                       ExecutionPlanSnapshot* out, std::string* error) {
  if (plan == nullptr || out == nullptr || limits.max_nodes == 0u ||
      limits.max_bytes == 0u) {
    if (error != nullptr) *error = "invalid app execution-plan snapshot request";
    return false;
  }

  orm_error_t orm_error;
  orm_error_init(&orm_error);
  orm_explain_mode_t mode = ORM_EXPLAIN_PLAN;
  orm_string_view_t provider{};
  orm_string_view_t raw{};
  std::uint64_t count = 0u;
  if (orm_execution_plan_mode(plan, &mode, &orm_error) != ORM_STATUS_OK ||
      orm_execution_plan_provider(plan, &provider, &orm_error) != ORM_STATUS_OK ||
      orm_execution_plan_node_count(plan, &count, &orm_error) != ORM_STATUS_OK ||
      orm_execution_plan_raw_detail(plan, &raw, &orm_error) != ORM_STATUS_OK) {
    if (error != nullptr) *error = orm_error.message;
    return false;
  }
  if (count == 0u || count > limits.max_nodes ||
      count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    if (error != nullptr) *error = "execution-plan node count exceeds app limit";
    return false;
  }

  ExecutionPlanSnapshot snapshot;
  snapshot.request_id = request_id;
  snapshot.mode = mode;
  snapshot.status = ORM_STATUS_OK;
  snapshot.elapsed_microseconds = elapsed_microseconds;
  if (!CopyView(provider, snapshot, limits, &snapshot.provider, error) ||
      !CopyView(raw, snapshot, limits, &snapshot.raw_detail, error))
    return false;

  snapshot.nodes.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0u; i < count; ++i) {
    orm_execution_plan_node_t node = ORM_EXECUTION_PLAN_NODE_INIT;
    if (orm_execution_plan_node(plan, i, &node, &orm_error) != ORM_STATUS_OK) {
      if (error != nullptr) *error = orm_error.message;
      return false;
    }
    if (node.parent_index != ORM_EXECUTION_PLAN_ROOT_INDEX &&
        node.parent_index >= i) {
      if (error != nullptr) *error = "execution-plan parent must precede child";
      return false;
    }

    ExecutionPlanNodeSnapshot copied;
    copied.flags = node.flags;
    copied.parent_index = node.parent_index;
    copied.ordinal = node.ordinal;
    copied.estimated_rows = node.estimated_rows;
    copied.actual_rows = node.actual_rows;
    copied.startup_cost = node.startup_cost;
    copied.total_cost = node.total_cost;
    copied.actual_startup_ms = node.actual_startup_ms;
    copied.actual_total_ms = node.actual_total_ms;
    if (!CopyView(node.node_type, snapshot, limits, &copied.node_type, error) ||
        !CopyView(node.relation, snapshot, limits, &copied.relation, error) ||
        !CopyView(node.index_name, snapshot, limits, &copied.index_name, error) ||
        !CopyView(node.detail, snapshot, limits, &copied.detail, error))
      return false;
    snapshot.nodes.push_back(std::move(copied));
  }

  *out = std::move(snapshot);
  return true;
}

}  // namespace turbodb::app
