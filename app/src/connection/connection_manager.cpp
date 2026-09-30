#include "connection/connection_manager.h"

#include <limits>
#include <utility>

namespace turbodb::app {

namespace {

void SetError(std::string* output, const orm_error_t& error,
              const char* fallback) {
  if (output == nullptr) return;
  *output = error.message[0] != '\0' ? error.message : fallback;
}

orm_string_view_t View(const std::string& value) {
  return {value.data(), value.size()};
}

}  // namespace

ConnectionManager::~ConnectionManager() {
  for (auto& [id, entry] : connections_) {
    (void)id;
    orm_disconnect(entry.connection);
    entry.connection = nullptr;
  }
  connections_.clear();
  if (runtime_ != nullptr) {
    orm_error_t error;
    orm_error_init(&error);
    (void)orm_runtime_close(runtime_, &error);
    orm_runtime_release(runtime_);
    runtime_ = nullptr;
  }
}

bool ConnectionManager::Initialize(std::string* error) {
  if (runtime_ != nullptr) return true;
  orm_runtime_config_t config;
  orm_runtime_config_init(&config);
  orm_error_t orm_error;
  orm_error_init(&orm_error);
  if (orm_runtime_create(&config, &runtime_, &orm_error) != ORM_STATUS_OK) {
    SetError(error, orm_error, "create TurboDB runtime");
    runtime_ = nullptr;
    return false;
  }
  return true;
}

SqlProvider ConnectionManager::ProviderFromId(const std::string& id) noexcept {
  if (id == "sqlite") return SqlProvider::sqlite;
  if (id == "mysql") return SqlProvider::mysql;
  if (id == "postgresql") return SqlProvider::postgresql;
  if (id == "tidesdb") return SqlProvider::tidesdb;
  return SqlProvider::unknown;
}

bool ConnectionManager::EnsureDriver(const ConnectionProfile& profile,
                                     orm_driver_info_t* out_info,
                                     std::string* error) {
  if (runtime_ == nullptr || out_info == nullptr || profile.driver_id.empty() ||
      profile.module_path.empty()) {
    if (error != nullptr) *error = "invalid TurboDB driver profile";
    return false;
  }

  auto existing = loaded_modules_.find(profile.driver_id);
  if (existing != loaded_modules_.end()) {
    if (existing->second != profile.module_path) {
      if (error != nullptr)
        *error = "driver id is already loaded from a different module path";
      return false;
    }
  } else {
    orm_driver_load_config_t load{};
    load.struct_size = static_cast<std::uint32_t>(sizeof(load));
    load.abi_version = ORM_RUNTIME_ABI_VERSION;
    load.module_path = View(profile.module_path);
    load.expected_driver_id = View(profile.driver_id);
    orm_error_t orm_error;
    orm_error_init(&orm_error);
    if (orm_runtime_load_driver(runtime_, &load, &orm_error) != ORM_STATUS_OK) {
      SetError(error, orm_error, "load TurboDB driver");
      return false;
    }
    loaded_modules_.emplace(profile.driver_id, profile.module_path);
  }

  *out_info = {};
  out_info->struct_size = static_cast<std::uint32_t>(sizeof(*out_info));
  out_info->abi_version = ORM_RUNTIME_ABI_VERSION;
  orm_error_t orm_error;
  orm_error_init(&orm_error);
  if (orm_runtime_driver_info(runtime_, View(profile.driver_id),
                              out_info, &orm_error) != ORM_STATUS_OK) {
    SetError(error, orm_error, "read TurboDB driver info");
    return false;
  }
  return true;
}

bool ConnectionManager::Open(const ConnectionProfile& profile,
                             WorkspaceConnectionIdentity* out_identity,
                             std::string* error) {
  if (out_identity == nullptr) {
    if (error != nullptr) *error = "connection identity output is null";
    return false;
  }
  *out_identity = {};
  if (!Initialize(error)) return false;

  orm_driver_info_t info{};
  if (!EnsureDriver(profile, &info, error)) return false;

  if (profile.options.size() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    if (error != nullptr) *error = "too many connection options";
    return false;
  }

  std::vector<orm_option_t> options;
  options.reserve(profile.options.size());
  for (const auto& option : profile.options) {
    if (option.keyword.empty()) {
      if (error != nullptr) *error = "connection option keyword is empty";
      return false;
    }
    options.push_back({View(option.keyword), View(option.value)});
  }

  orm_config_t config;
  orm_config(&config);
  config.driver = View(profile.driver_id);
  config.options = options.empty() ? nullptr : options.data();
  config.option_count = static_cast<std::uint32_t>(options.size());

  orm_connection_t* connection = nullptr;
  orm_error_t orm_error;
  orm_error_init(&orm_error);
  if (orm_runtime_connect(runtime_, &config, &connection, &orm_error) !=
      ORM_STATUS_OK) {
    SetError(error, orm_error, "connect TurboDB driver");
    return false;
  }

  if (next_id_ == 0u || connections_.find(next_id_) != connections_.end()) {
    orm_disconnect(connection);
    if (error != nullptr) *error = "connection id space exhausted";
    return false;
  }

  WorkspaceConnectionIdentity identity;
  identity.id = next_id_++;
  identity.provider = ProviderFromId(profile.driver_id);
  identity.capabilities = info.capabilities;
  identity.display_name = profile.display_name.empty()
                              ? profile.driver_id
                              : profile.display_name;

  connections_.emplace(identity.id, Entry{identity, connection});
  *out_identity = identity;
  return true;
}

bool ConnectionManager::Close(std::uint64_t id, std::string* error) {
  auto found = connections_.find(id);
  if (found == connections_.end()) {
    if (error != nullptr) *error = "connection id is not open";
    return false;
  }

  orm_error_t orm_error;
  orm_error_init(&orm_error);
  if (orm_connection_close(found->second.connection, &orm_error) !=
      ORM_STATUS_OK) {
    SetError(error, orm_error, "close TurboDB connection");
    return false;
  }
  orm_connection_release(found->second.connection);
  connections_.erase(found);
  return true;
}

orm_connection_t* ConnectionManager::Get(std::uint64_t id) const noexcept {
  auto found = connections_.find(id);
  return found == connections_.end() ? nullptr : found->second.connection;
}

std::optional<WorkspaceConnectionIdentity> ConnectionManager::Identity(
    std::uint64_t id) const {
  auto found = connections_.find(id);
  if (found == connections_.end()) return std::nullopt;
  return found->second.identity;
}

}  // namespace turbodb::app
