#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>

#include "connection/connection_manager.h"
#include "query/query_result_model.h"

namespace turbodb::app {

constexpr UINT kQueryExecutionCompleted = WM_APP + 0x121;

class QueryController {
 public:
  explicit QueryController(ConnectionManager& connections) noexcept
      : connections_(connections) {}
  QueryController(const QueryController&) = delete;
  QueryController& operator=(const QueryController&) = delete;
  ~QueryController();

  bool Execute(std::uint64_t connection_id, std::string sql,
               HWND notify_window, std::uint64_t* out_request_id,
               std::string* error);
  bool RequestCancel(std::string* error) const;
  bool busy() const noexcept;
  std::shared_ptr<const QueryResultSnapshot> TakeCompleted(
      std::uint64_t request_id);

 private:
  void Run(std::uint64_t request_id, orm_connection_t* connection,
           std::string sql, HWND notify_window);

  ConnectionManager& connections_;
  QueryResultLimits limits_;
  mutable std::mutex mutex_;
  std::thread worker_;
  std::shared_ptr<QueryResultSnapshot> completed_;
  std::uint64_t next_request_id_ = 1u;
  std::uint64_t active_request_id_ = 0u;
  bool busy_ = false;
};

}  // namespace turbodb::app
