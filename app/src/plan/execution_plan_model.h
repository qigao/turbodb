#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <orm.h>

namespace turbodb::app {

struct ExecutionPlanNodeSnapshot {
  std::uint32_t flags = 0u;
  std::uint64_t parent_index = ORM_EXECUTION_PLAN_ROOT_INDEX;
  std::uint64_t ordinal = 0u;
  std::string node_type;
  std::string relation;
  std::string index_name;
  std::string detail;
  double estimated_rows = 0.0;
  double actual_rows = 0.0;
  double startup_cost = 0.0;
  double total_cost = 0.0;
  double actual_startup_ms = 0.0;
  double actual_total_ms = 0.0;
};

struct ExecutionPlanSnapshot {
  std::uint64_t request_id = 0u;
  orm_explain_mode_t mode = ORM_EXPLAIN_PLAN;
  orm_status_t status = ORM_STATUS_OK;
  std::string message;
  std::string provider;
  std::string raw_detail;
  std::uint64_t elapsed_microseconds = 0u;
  std::uint64_t payload_bytes = 0u;
  std::vector<ExecutionPlanNodeSnapshot> nodes;
};

struct ExecutionPlanCopyLimits {
  std::uint64_t max_nodes = ORM_C_DEFAULT_MAX_RESULT_ROWS;
  std::uint64_t max_bytes = ORM_C_DEFAULT_MAX_RESULT_BYTES;
};

bool CopyExecutionPlan(orm_execution_plan_t* plan, std::uint64_t request_id,
                       std::uint64_t elapsed_microseconds,
                       const ExecutionPlanCopyLimits& limits,
                       ExecutionPlanSnapshot* out, std::string* error);

}  // namespace turbodb::app
