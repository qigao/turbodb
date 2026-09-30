#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "connection/connection_manager.h"
#include "query/query_controller.h"

namespace {

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

std::shared_ptr<const turbodb::app::QueryResultSnapshot> WaitFor(
    turbodb::app::QueryController& controller, std::uint64_t request_id) {
  for (int attempt = 0; attempt < 5000; ++attempt) {
    if (!controller.busy())
      return controller.TakeCompleted(request_id);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return {};
}

bool Execute(turbodb::app::QueryController& controller, std::uint64_t id,
             const char* sql,
             std::shared_ptr<const turbodb::app::QueryResultSnapshot>* out) {
  std::string error;
  std::uint64_t request_id = 0u;
  if (!controller.Execute(id, sql, nullptr, &request_id, &error)) {
    std::cerr << "execute start failed: " << error << '\n';
    return false;
  }
  *out = WaitFor(controller, request_id);
  return static_cast<bool>(*out);
}

}  // namespace

int main() {
  using namespace turbodb::app;

  ConnectionManager connections;
  ConnectionProfile profile;
  profile.driver_id = "sqlite";
  profile.module_path = TURBODB_APP_SQLITE_PLUGIN_PATH;
  profile.display_name = "query-test";
  profile.options.push_back({"filename", ":memory:"});

  WorkspaceConnectionIdentity identity;
  std::string error;
  if (!connections.Open(profile, &identity, &error))
    return Fail("open SQLite connection");

  QueryController controller(connections);
  std::shared_ptr<const QueryResultSnapshot> result;

  if (!Execute(controller, identity.id,
               "create table users(id integer primary key, name text not null)",
               &result))
    return Fail("create table request did not complete");
  if (result->kind != QueryOutcomeKind::command ||
      result->status != ORM_STATUS_OK)
    return Fail("DDL must produce a successful command result");

  if (!Execute(controller, identity.id,
               "insert into users(id, name) values (1, 'Ada'), (2, 'Linus')",
               &result))
    return Fail("insert request did not complete");
  if (result->kind != QueryOutcomeKind::command ||
      result->status != ORM_STATUS_OK || result->affected_rows != 2u)
    return Fail("DML affected-row result mismatch");

  if (!Execute(controller, identity.id,
               "select id, name from users order by id", &result))
    return Fail("select request did not complete");
  if (result->kind != QueryOutcomeKind::rows ||
      result->status != ORM_STATUS_OK || result->columns.size() != 2u ||
      result->rows.size() != 2u)
    return Fail("SELECT result shape mismatch");
  if (result->columns[0] != "id" || result->columns[1] != "name")
    return Fail("SELECT column names were not preserved");
  if (result->rows[0][0].kind != ORM_VALUE_INT64 ||
      result->rows[0][0].int64_value != 1 ||
      result->rows[0][1].kind != ORM_VALUE_TEXT ||
      result->rows[0][1].bytes != "Ada" ||
      result->rows[1][0].int64_value != 2 ||
      result->rows[1][1].bytes != "Linus")
    return Fail("SELECT typed row values mismatch");

  orm_connection_t* native = connections.Get(identity.id);
  if (native == nullptr)
    return Fail("active SQLite connection disappeared");

  orm_error_t plan_error;
  orm_error_init(&plan_error);
  orm_execution_plan_t* plan = nullptr;
  if (orm_query_explain(
          native, orm_view("select id, name from users order by id"),
          ORM_EXPLAIN_PLAN, &plan, &plan_error) != ORM_STATUS_OK)
    return Fail("SQLite execution PLAN acquisition failed");

  uint64_t plan_nodes = 0u;
  orm_string_view_t plan_provider{};
  orm_string_view_t raw_plan{};
  if (orm_execution_plan_node_count(plan, &plan_nodes, &plan_error) !=
          ORM_STATUS_OK ||
      orm_execution_plan_provider(plan, &plan_provider, &plan_error) !=
          ORM_STATUS_OK ||
      orm_execution_plan_raw_detail(plan, &raw_plan, &plan_error) !=
          ORM_STATUS_OK ||
      plan_nodes == 0u || raw_plan.len == 0u ||
      std::string(static_cast<const char*>(plan_provider.data),
                  plan_provider.len) != "sqlite") {
    orm_execution_plan_destroy(plan);
    return Fail("SQLite normalized execution PLAN shape mismatch");
  }
  orm_execution_plan_destroy(plan);
  plan = nullptr;

  if (orm_query_explain(
          native, orm_view("select id from users"),
          ORM_EXPLAIN_ANALYZE, &plan, &plan_error) !=
          ORM_STATUS_UNSUPPORTED ||
      plan != nullptr)
    return Fail("SQLite ANALYZE must be explicitly unsupported");

  if (!Execute(controller, identity.id, "select from", &result))
    return Fail("invalid SQL request did not complete");
  if (result->kind != QueryOutcomeKind::error ||
      result->status == ORM_STATUS_OK || result->message.empty())
    return Fail("SQL error status/message was not preserved");

  std::string cancel_error;
  if (controller.RequestCancel(&cancel_error) || cancel_error.empty())
    return Fail("idle cancellation contract mismatch");

  if (!connections.Close(identity.id, &error))
    return Fail("close SQLite connection");
  return 0;
}
