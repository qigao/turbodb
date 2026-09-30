#include "query/query_controller.h"

#include <chrono>
#include <utility>

namespace turbodb::app {

namespace {

std::shared_ptr<QueryResultSnapshot> ErrorSnapshot(
    std::uint64_t request_id, orm_status_t status, const char* message,
    std::uint64_t elapsed_microseconds) {
  auto result = std::make_shared<QueryResultSnapshot>();
  result->request_id = request_id;
  result->kind = QueryOutcomeKind::error;
  result->status = status;
  result->message =
      message != nullptr && message[0] != '\0'
          ? message
          : orm_status_message(status);
  result->elapsed_microseconds = elapsed_microseconds;
  result->cancel_supported = false;
  return result;
}

}  // namespace

QueryController::~QueryController() {
  if (worker_.joinable()) worker_.join();
}

bool QueryController::busy() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return busy_;
}

bool QueryController::has_pending_completion() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return completed_ != nullptr;
}

bool QueryController::Execute(std::uint64_t connection_id, std::string sql,
                              HWND notify_window,
                              std::uint64_t* out_request_id,
                              std::string* error) {
  if (out_request_id != nullptr) *out_request_id = 0u;
  if (sql.empty() || out_request_id == nullptr) {
    if (error != nullptr) *error = "SQL text is empty";
    return false;
  }

  orm_connection_t* connection = connections_.Get(connection_id);
  if (connection == nullptr) {
    if (error != nullptr) *error = "active TurboDB connection is unavailable";
    return false;
  }
  const orm_status_t retain_status = orm_connection_retain(connection);
  if (retain_status != ORM_STATUS_OK) {
    if (error != nullptr) *error = orm_status_message(retain_status);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_) {
      orm_connection_release(connection);
      if (error != nullptr) *error = "a SQL request is already running";
      return false;
    }
    if (completed_ != nullptr) {
      orm_connection_release(connection);
      if (error != nullptr)
        *error = "the previous SQL result is pending UI consumption";
      return false;
    }
  }
  if (worker_.joinable()) worker_.join();

  std::uint64_t request_id = 0u;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_request_id_ == 0u) {
      orm_connection_release(connection);
      if (error != nullptr) *error = "query request id space exhausted";
      return false;
    }
    request_id = next_request_id_++;
    active_request_id_ = request_id;
    busy_ = true;
    completed_.reset();
  }

  try {
    worker_ = std::thread(&QueryController::Run, this, request_id, connection,
                          std::move(sql), notify_window);
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
    active_request_id_ = 0u;
    orm_connection_release(connection);
    if (error != nullptr) *error = "create SQL execution worker";
    return false;
  }

  *out_request_id = request_id;
  return true;
}

bool QueryController::RequestCancel(std::string* error) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!busy_) {
    if (error != nullptr) *error = "there is no active SQL request";
    return false;
  }
  if (error != nullptr) {
    *error =
        "active materialized execution has no public cancellation handle; "
        "TurboDB will not pretend the query was cancelled or rolled back";
  }
  return false;
}

std::shared_ptr<const QueryResultSnapshot> QueryController::TakeCompleted(
    std::uint64_t request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (completed_ == nullptr || completed_->request_id != request_id)
    return {};
  auto result = std::move(completed_);
  completed_.reset();
  return result;
}

void QueryController::Run(std::uint64_t request_id,
                          orm_connection_t* connection, std::string sql,
                          HWND notify_window) {
  const auto started = std::chrono::steady_clock::now();
  orm_error_t error;
  orm_error_init(&error);
  orm_query_t* query = nullptr;
  orm_result_t* result = nullptr;

  const orm_string_view_t sql_view{sql.data(), sql.size()};
  orm_status_t status = orm_raw(connection, sql_view, &query, &error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, &error);

  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started);
  std::shared_ptr<QueryResultSnapshot> snapshot;
  if (status != ORM_STATUS_OK) {
    snapshot = ErrorSnapshot(request_id, status, error.message,
                             static_cast<std::uint64_t>(elapsed.count()));
  } else {
    auto output = std::make_shared<QueryResultSnapshot>();
    std::string copy_error;
    if (!CopyOrmResult(result, request_id,
                       static_cast<std::uint64_t>(elapsed.count()), limits_,
                       output.get(), &copy_error)) {
      snapshot = ErrorSnapshot(
          request_id, ORM_STATUS_LIMIT_EXCEEDED, copy_error.c_str(),
          static_cast<std::uint64_t>(elapsed.count()));
    } else {
      snapshot = std::move(output);
    }
  }

  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_connection_release(connection);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    completed_ = std::move(snapshot);
    busy_ = false;
    active_request_id_ = 0u;
  }
  if (notify_window != nullptr && ::IsWindow(notify_window))
    (void)::PostMessageW(notify_window, kQueryExecutionCompleted,
                         static_cast<WPARAM>(request_id), 0);
}

}  // namespace turbodb::app
