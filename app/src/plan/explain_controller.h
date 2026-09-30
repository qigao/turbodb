#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <winsock2.h>
#include <windows.h>

#include "connection/connection_manager.h"
#include "plan/execution_plan_model.h"

namespace turbodb::app {

constexpr UINT kExplainExecutionCompleted = WM_APP + 0x124;

class ExplainController {
 public:
  explicit ExplainController(ConnectionManager& connections) noexcept
      : connections_(connections) {}
  ExplainController(const ExplainController&) = delete;
  ExplainController& operator=(const ExplainController&) = delete;
  ~ExplainController();

  bool Execute(std::uint64_t connection_id, std::string sql,
               orm_explain_mode_t mode, HWND notify_window,
               std::uint64_t* out_request_id, std::string* error);
  bool busy() const noexcept;
  bool has_pending_completion() const noexcept;
  std::shared_ptr<const ExecutionPlanSnapshot> TakeCompleted(
      std::uint64_t request_id);

 private:
  void Run(std::uint64_t request_id, orm_connection_t* connection,
           std::string sql, orm_explain_mode_t mode, HWND notify_window);

  ConnectionManager& connections_;
  ExecutionPlanCopyLimits limits_;
  mutable std::mutex mutex_;
  std::thread worker_;
  std::shared_ptr<ExecutionPlanSnapshot> completed_;
  std::uint64_t next_request_id_ = 1u;
  bool busy_ = false;
};

}  // namespace turbodb::app
