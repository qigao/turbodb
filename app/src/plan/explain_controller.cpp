#include "plan/explain_controller.h"

#include <chrono>
#include <utility>

namespace turbodb::app {

namespace {

std::shared_ptr<ExecutionPlanSnapshot> ErrorSnapshot(
    std::uint64_t request_id, orm_explain_mode_t mode,
    orm_status_t status, const char* message,
    std::uint64_t elapsed_microseconds) {
  auto result = std::make_shared<ExecutionPlanSnapshot>();
  result->request_id = request_id;
  result->mode = mode;
  result->status = status;
  result->message = message != nullptr && message[0] != '\0'
                        ? message
                        : orm_status_message(status);
  result->elapsed_microseconds = elapsed_microseconds;
  return result;
}

}  // namespace

ExplainController::~ExplainController() {
  if (worker_.joinable()) worker_.join();
}

bool ExplainController::busy() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return busy_;
}

bool ExplainController::has_pending_completion() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return completed_ != nullptr;
}

bool ExplainController::Execute(std::uint64_t connection_id, std::string sql,
                                orm_explain_mode_t mode, HWND notify_window,
                                std::uint64_t* out_request_id,
                                std::string* error) {
  if (out_request_id != nullptr) *out_request_id = 0u;
  if (out_request_id == nullptr || sql.empty() ||
      (mode != ORM_EXPLAIN_PLAN && mode != ORM_EXPLAIN_ANALYZE)) {
    if (error != nullptr) *error = "invalid execution-plan request";
    return false;
  }

  orm_connection_t* connection = connections_.Get(connection_id);
  if (connection == nullptr) {
    if (error != nullptr) *error = "active TurboDB connection is unavailable";
    return false;
  }
  const orm_status_t retained = orm_connection_retain(connection);
  if (retained != ORM_STATUS_OK) {
    if (error != nullptr) *error = orm_status_message(retained);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_) {
      orm_connection_release(connection);
      if (error != nullptr) *error = "an execution-plan request is already running";
      return false;
    }
    if (completed_ != nullptr) {
      orm_connection_release(connection);
      if (error != nullptr) *error = "the previous execution plan is pending UI consumption";
      return false;
    }
  }
  if (worker_.joinable()) worker_.join();

  std::uint64_t request_id = 0u;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_request_id_ == 0u) {
      orm_connection_release(connection);
      if (error != nullptr) *error = "execution-plan request id space exhausted";
      return false;
    }
    request_id = next_request_id_++;
    busy_ = true;
  }

  try {
    worker_ = std::thread(&ExplainController::Run, this, request_id, connection,
                          std::move(sql), mode, notify_window);
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
    orm_connection_release(connection);
    if (error != nullptr) *error = "create execution-plan worker";
    return false;
  }
  *out_request_id = request_id;
  return true;
}

std::shared_ptr<const ExecutionPlanSnapshot> ExplainController::TakeCompleted(
    std::uint64_t request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (completed_ == nullptr || completed_->request_id != request_id)
    return {};
  auto result = std::move(completed_);
  completed_.reset();
  return result;
}

void ExplainController::Run(std::uint64_t request_id,
                            orm_connection_t* connection, std::string sql,
                            orm_explain_mode_t mode, HWND notify_window) {
  const auto started = std::chrono::steady_clock::now();
  orm_error_t error;
  orm_error_init(&error);
  orm_execution_plan_t* plan = nullptr;
  const orm_string_view_t sql_view{sql.data(), sql.size()};
  const orm_status_t status =
      orm_query_explain(connection, sql_view, mode, &plan, &error);
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started);

  std::shared_ptr<ExecutionPlanSnapshot> snapshot;
  if (status != ORM_STATUS_OK) {
    snapshot = ErrorSnapshot(request_id, mode, status, error.message,
                             static_cast<std::uint64_t>(elapsed.count()));
  } else {
    auto output = std::make_shared<ExecutionPlanSnapshot>();
    std::string copy_error;
    if (!CopyExecutionPlan(plan, request_id,
                           static_cast<std::uint64_t>(elapsed.count()),
                           limits_, output.get(), &copy_error)) {
      snapshot = ErrorSnapshot(request_id, mode, ORM_STATUS_LIMIT_EXCEEDED,
                               copy_error.c_str(),
                               static_cast<std::uint64_t>(elapsed.count()));
    } else {
      snapshot = std::move(output);
    }
  }

  orm_execution_plan_destroy(plan);
  orm_connection_release(connection);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    completed_ = std::move(snapshot);
    busy_ = false;
  }
  if (notify_window != nullptr && ::IsWindow(notify_window))
    (void)::PostMessageW(notify_window, kExplainExecutionCompleted,
                         static_cast<WPARAM>(request_id), 0);
}

}  // namespace turbodb::app
