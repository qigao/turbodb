#include <orm.hpp>

#include <cmeta/struct.h>
#include <tinytest.hpp>
#include <salts/thread.h>

#include <cstddef>
#include <utility>

#define ORM_CPP_FLOW_DATA_PREFIX_SIZE                                        \
  (offsetof(cmeta_data_desc, shape) +                                        \
   sizeof(((cmeta_data_desc *)0)->shape))

Struct(orm_cpp_flow_row,
    (int, id),
    (long, score)
);

static const cmeta_type_identity orm_cpp_flow_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.CppFlowRow");
static const cmeta_type_traits orm_cpp_flow_row_traits = {
    CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static const cmeta_type_desc orm_cpp_flow_row_type = {
    "orm_cpp_flow_row", sizeof(orm_cpp_flow_row), alignof(orm_cpp_flow_row),
    CMETA_T_OBJECT, nullptr, &orm_cpp_flow_row_traits,
    &orm_cpp_flow_row_identity};
static const cmeta_data_field_desc orm_cpp_flow_row_fields[] = {
    {"orm.test.CppFlowRow.id", "id", offsetof(orm_cpp_flow_row, id),
     &cmeta_data_int},
    {"orm.test.CppFlowRow.score", "score", offsetof(orm_cpp_flow_row, score),
     &cmeta_data_long}};
static const cmeta_data_struct_shape orm_cpp_flow_row_shape = {
    StructMeta(orm_cpp_flow_row), orm_cpp_flow_row_fields, 2u};
static const cmeta_data_desc orm_cpp_flow_row_data = {
    ORM_CPP_FLOW_DATA_PREFIX_SIZE,
    CMETA_DATA_DESC_ABI_VERSION,
    "orm.test.CppFlowRow.data",
    "CppFlowRow",
    CMETA_DATA_STRUCT,
    &orm_cpp_flow_row_type,
    &orm_cpp_flow_row_shape,
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr};

static const cmeta_type_desc row_pointer_type = {
    "orm_cpp_flow_row *", sizeof(orm_cpp_flow_row *), alignof(orm_cpp_flow_row *),
    CMETA_T_POINTER, &orm_cpp_flow_row_type, nullptr, nullptr};
CMETA_FUNCTION_METADATA_AS_ABI(row_selected, "row_selected", value, &cmeta_type_bool, CMETA_ABI_SCALAR,
    (orm_cpp_flow_row *, row, CMETA_PARAM_IN, &row_pointer_type, CMETA_ABI_OBJECT_POINTER));
CMETA_FUNCTION_METADATA_AS_ABI(row_increment, "row_increment", fallible, &cmeta_type_int, CMETA_ABI_SCALAR,
    (orm_cpp_flow_row *, row, CMETA_PARAM_IN, &row_pointer_type, CMETA_ABI_OBJECT_POINTER),
    (orm_cpp_flow_row *, result, CMETA_PARAM_OUT, &row_pointer_type, CMETA_ABI_OBJECT_POINTER));
bool row_selected(orm_cpp_flow_row *row) { return row->id > 7; }
int row_increment(orm_cpp_flow_row *row, orm_cpp_flow_row *result) {
  *result = *row;
  ++result->id;
  return 0;
}
static bool select_adapter(const cmeta_callable *, void *out, const void *const *args) {
  *static_cast<bool *>(out) = row_selected(const_cast<orm_cpp_flow_row *>(
      static_cast<const orm_cpp_flow_row *>(args[0])));
  return true;
}
static bool increment_adapter(const cmeta_callable *, void *out, const void *const *args) {
  return row_increment(const_cast<orm_cpp_flow_row *>(
      static_cast<const orm_cpp_flow_row *>(args[0])), static_cast<orm_cpp_flow_row *>(out)) == 0;
}
static cmeta_callable row_adapter(const cmeta_function_desc *function, cmeta_callable_invoke_fn invoke) {
  cmeta_callable adapter{};
  adapter.meta.effects = function->effects;
  adapter.meta.properties = function->properties;
  adapter.dispatch = CMETA_CALLABLE_DISPATCH_ADAPTER;
  adapter.invoke = invoke;
  return adapter;
}

namespace {
struct flow_observer {
  int count = 0;
  int last_id = 0;
  int completed = 0;
  int failed = 0;
  cflow_subscriber_callbacks callbacks{
      [](void *user, const cmeta_type_desc *, const void *value) {
        auto &self = *static_cast<flow_observer *>(user);
        ++self.count;
        self.last_id = static_cast<const orm_cpp_flow_row *>(value)->id;
        return true;
      },
      [](void *user, const char *) { ++static_cast<flow_observer *>(user)->failed; },
      [](void *user) { ++static_cast<flow_observer *>(user)->completed; }, this};
  cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
};
struct flow_scheduler {
  cflow_scheduler native{};
  flow_scheduler() {
    if (!cflow_scheduler_test_init(&native)) throw std::runtime_error("scheduler init");
  }
  ~flow_scheduler() { cflow_scheduler_destroy(&native); }
  void drain() { (void)cflow_scheduler_run_until_idle(&native, 0u); }
};
orm::flow make_flow(orm::connection &connection) {
  return connection.raw("select 7 as id, 19 as score union all select 11, 29")
      .open<orm_cpp_flow_row>(orm_cpp_flow_row_data).pipe();
}
}

spec("ORM thin C++ CFlow facade") {
  it("runs typed row filter and map with demand preserved across rejected rows") {
    cflow_function_typed_adapter_projection filter{}, map{};
    check_equal(cflow_function_typed_filter_projection_admit(&row_selected__function_meta,
        &row_selected__function_abi_meta, row_adapter(&row_selected__function_meta, select_adapter),
        &orm_cpp_flow_row_type, &filter), CFLOW_FUNCTION_PROJECTION_OK);
    check_equal(cflow_function_typed_adapter_projection_admit(&row_increment__function_meta,
        &row_increment__function_abi_meta, row_adapter(&row_increment__function_meta, increment_adapter),
        &orm_cpp_flow_row_type, &orm_cpp_flow_row_type, &map), CFLOW_FUNCTION_PROJECTION_OK);
    orm::connection connection(orm::config("sqlite").option("filename", ":memory:"));
    flow_scheduler scheduler;
    flow_observer observer;
    auto run = make_flow(connection).filter(filter).map(map).take(1u)
        .subscribe(scheduler.native, observer.sink);
    check_equal(run.request(1u).status, CFLOW_STATUS_OK);
    scheduler.drain();
    check_equal(observer.count, 1);
    check_equal(observer.last_id, 12);
    check_equal(observer.completed, 1);
    check_equal(observer.failed, 0);
  }

  it("keeps a pipeline usable after subscription admission fails") {
    orm::connection connection(orm::config("sqlite").option("filename", ":memory:"));
    flow_scheduler scheduler;
    flow_observer observer;
    cflow_scheduler invalid{};
    auto pipeline = make_flow(connection);
    bool rejected = false;
    try { (void)pipeline.subscribe(invalid, observer.sink); }
    catch (const orm::flow_error &) { rejected = true; }
    check_true(rejected);
    auto run = pipeline.take(1u).subscribe(scheduler.native, observer.sink);
    check_equal(run.request(1u).status, CFLOW_STATUS_OK);
    scheduler.drain();
    check_equal(observer.count, 1);
  }

  it("rejects invalid async configuration without consuming the query") {
    orm::connection connection(orm::config("sqlite").option("filename", ":memory:"));
    flow_scheduler scheduler;
    orm_async_config_t async{sizeof(orm_async_config_t), &scheduler.native, 1u, 0u};
    auto query = connection.raw("select 7 as id, 19 as score");
    bool rejected = false;
    try { (void)std::move(query).open_async<orm_cpp_flow_row>(orm_cpp_flow_row_data, async); }
    catch (const orm::status_error &) { rejected = true; }
    check_true(rejected);
    auto rows = std::move(query).open<orm_cpp_flow_row>(orm_cpp_flow_row_data);
    orm_cpp_flow_row row{};
    check_equal(rows.next(row).kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
  }

  it("runs SQLite async through the C++ pipeline with bounded demand") {
    enum { max_pumps = 5000, timeout_ticks = 10000 };
    orm::connection connection(orm::config("sqlite").option("filename", ":memory:"));
    flow_scheduler scheduler;
    flow_observer observer;
    orm_async_config_t async{sizeof(async), &scheduler.native, 1u, timeout_ticks};
    auto run = connection.raw("select 7 as id, 19 as score union all select 11, 29")
        .open_async<orm_cpp_flow_row>(orm_cpp_flow_row_data, async).pipe().take(2u)
        .subscribe(scheduler.native, observer.sink);
    (void)cflow_scheduler_run_ready(&scheduler.native);
    check_equal(observer.count, 0);
    for (int expected = 1; expected <= 2; ++expected) {
      check_equal(run.request(1u).status, CFLOW_STATUS_OK);
      for (int attempt = 0; attempt < max_pumps && observer.count < expected && !observer.failed;
           ++attempt) {
        (void)cflow_scheduler_advance(&scheduler.native, 1u);
        salts_sleep_ms(1u);
      }
      check_equal(observer.count, expected);
      check_equal(observer.last_id, expected == 1 ? 7 : 11);
      (void)cflow_scheduler_advance(&scheduler.native, 1u);
      check_equal(observer.count, expected);
    }
    check_equal(observer.failed, 0);
    check_equal(observer.completed, 1);
    run.close();
    auto rows = connection.raw("select 23 as id, 31 as score")
        .open<orm_cpp_flow_row>(orm_cpp_flow_row_data);
    orm_cpp_flow_row row{};
    check_equal(rows.next(row).kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 23);
  }

  it("keeps temporary publisher and graph alive until subscription close") {
    orm::connection connection(orm::config("sqlite").option("filename", ":memory:"));
    flow_scheduler scheduler;
    flow_observer observer;
    auto run = make_flow(connection).take(1u).subscribe(scheduler.native, observer.sink);
    scheduler.drain();
    check_equal(observer.count, 0);
    check_equal(run.request(1u).status, CFLOW_STATUS_OK);
    scheduler.drain();
    check_equal(observer.count, 1);
    check_equal(observer.last_id, 7);
    check_equal(observer.completed, 1);
    check_equal(observer.failed, 0);
    check_true(run.done());
    run.close();
    run.close();
    check_equal(run.request(1u).status, CFLOW_STATUS_INVALID_ARGUMENT);
  }

  it("preserves graph addresses when moving an active subscription") {
    orm::connection connection(orm::config("sqlite").option("filename", ":memory:"));
    flow_scheduler scheduler;
    flow_observer observer;
    auto first = make_flow(connection).subscribe(scheduler.native, observer.sink);
    auto moved = std::move(first);
    check_equal(moved.request(1u).status, CFLOW_STATUS_OK);
    scheduler.drain();
    check_equal(observer.count, 1);
    check_equal(observer.completed, 0);
    check_equal(moved.request(2u).status, CFLOW_STATUS_OK);
    scheduler.drain();
    check_equal(observer.count, 2);
    check_equal(observer.last_id, 11);
    check_equal(observer.completed, 1);
  }

  it("cancels pending demand before scheduler delivery") {
    orm::connection connection(orm::config("sqlite").option("filename", ":memory:"));
    flow_scheduler scheduler;
    flow_observer observer;
    auto run = make_flow(connection).subscribe(scheduler.native, observer.sink);
    check_equal(run.request(1u).status, CFLOW_STATUS_OK);
    run.cancel();
    scheduler.drain();
    check_equal(observer.count, 0);
    check_equal(run.status(), CFLOW_STATUS_CANCELLED);
  }

  it("rejects resubscribing a consumed pipeline") {
    orm::connection connection(orm::config("sqlite").option("filename", ":memory:"));
    flow_scheduler scheduler;
    flow_observer observer;
    auto pipeline = make_flow(connection);
    auto run = pipeline.subscribe(scheduler.native, observer.sink);
    bool rejected = false;
    try { (void)pipeline.subscribe(scheduler.native, observer.sink); }
    catch (const orm::flow_error &) { rejected = true; }
    check_true(rejected);
  }

  it("only owns C handles and forwards typed Publisher demand") {
    orm::connection connection(orm::config("sqlite").option("filename", ":memory:"));
    orm::publisher<orm_cpp_flow_row> rows = std::move(connection.raw(
        "select 7 as id, 19 as score union all select 11, 29 order by id"))
                    .open<orm_cpp_flow_row>(orm_cpp_flow_row_data);
    cflow_publish_context context{};
    orm_cpp_flow_row first{};
    orm_cpp_flow_row second{};
    check_equal(rows.next(first, context).kind, CFLOW_STEP_VALUE);
    check_equal(first.id, 7);
    check_equal(first.score, 19L);
    check_equal(rows.next(second).kind, CFLOW_STEP_VALUE);
    check_equal(second.id, 11);
    check_equal(second.score, 29L);
    check_equal(rows.next(second).kind, CFLOW_STEP_DONE);
  }

  it("forwards command demand and affected rows through the C Publisher") {
    orm::connection connection(orm::config("sqlite").option("filename", ":memory:"));
    orm::publisher<orm_command_result_t> create = std::move(connection.raw(
        "create table cpp_flow_command(id integer, score integer)"))
                      .execute();
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(create.next(result).kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(result.affected_rows, uint64_t{0});

    auto insert = std::move(connection.raw(
        "insert into cpp_flow_command values(7, 19)"))
                      .execute();
    result = ORM_COMMAND_RESULT_INIT;
    check_equal(insert.next(result).kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(result.affected_rows, uint64_t{1});

    orm::publisher<orm_cpp_flow_row> rows = std::move(connection.raw(
        "select id, score from cpp_flow_command"))
                    .open<orm_cpp_flow_row>(orm_cpp_flow_row_data);
    orm_cpp_flow_row row{};
    check_equal(rows.next(row).kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
    check_equal(row.score, 19L);
  }
}
