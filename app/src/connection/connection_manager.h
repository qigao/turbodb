#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <orm_runtime.h>

#include "workspace/sql_workspace_session.h"

namespace turbodb::app {

struct ConnectionOption {
  std::string keyword;
  std::string value;
};

struct ConnectionProfile {
  std::string driver_id;
  std::string module_path;
  std::string display_name;
  std::vector<ConnectionOption> options;
};

class ConnectionManager {
 public:
  ConnectionManager() = default;
  ConnectionManager(const ConnectionManager&) = delete;
  ConnectionManager& operator=(const ConnectionManager&) = delete;
  ~ConnectionManager();

  bool Initialize(std::string* error);
  bool Open(const ConnectionProfile& profile,
            WorkspaceConnectionIdentity* out_identity,
            std::string* error);
  bool Close(std::uint64_t id, std::string* error);

  orm_connection_t* Get(std::uint64_t id) const noexcept;
  std::optional<WorkspaceConnectionIdentity> Identity(
      std::uint64_t id) const;

 private:
  struct Entry {
    WorkspaceConnectionIdentity identity;
    orm_connection_t* connection = nullptr;
  };

  static SqlProvider ProviderFromId(const std::string& id) noexcept;
  bool EnsureDriver(const ConnectionProfile& profile,
                    orm_driver_info_t* out_info,
                    std::string* error);

  orm_runtime_t* runtime_ = nullptr;
  std::map<std::string, std::string> loaded_modules_;
  std::map<std::uint64_t, Entry> connections_;
  std::uint64_t next_id_ = 1u;
};

}  // namespace turbodb::app
