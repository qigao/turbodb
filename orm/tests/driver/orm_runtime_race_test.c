#include "orm_module_loader.h"
#include <orm_runtime.h>

#include <salts/thread.h>
#include <tinytest.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RACE_TIMEOUT_MS UINT64_C(5000)
enum {
  RACE_GATE_INITIALIZE = 1u,
  RACE_GATE_CONNECT = 2u,
  RACE_GATE_FINALIZE = 3u
};

typedef int32_t (ORM_DRIVER_CALL *race_gate_arm_fn)(uint32_t);
typedef int32_t (ORM_DRIVER_CALL *race_gate_wait_fn)(uint64_t);
typedef void (ORM_DRIVER_CALL *race_gate_release_fn)(void);

typedef struct race_control {
  orm_module_handle module;
  race_gate_arm_fn arm;
  race_gate_wait_fn wait_entered;
  race_gate_release_fn release;
} race_control;

typedef struct race_worker {
  orm_runtime_t *runtime;
  orm_driver_load_config_t load;
  orm_config_t connect;
  orm_connection_t *connection;
  orm_error_t error;
  orm_status_t status;
} race_worker;

static const char *fixture_path(void) {
  const char *path = getenv("ORM_RUNTIME_RACE_FIXTURE");
  return path == NULL ? "" : path;
}

static orm_driver_load_config_t load_config(void) {
  orm_driver_load_config_t config;
  memset(&config, 0, sizeof(config));
  config.struct_size = (uint32_t)sizeof(config);
  config.abi_version = ORM_RUNTIME_ABI_VERSION;
  config.module_path = orm_view(fixture_path());
  config.expected_driver_id = orm_view("race");
  return config;
}

static void load_worker(void *context) {
  race_worker *worker = context;
  orm_error_init(&worker->error);
  worker->status =
      orm_runtime_load_driver(worker->runtime, &worker->load, &worker->error);
}

static void connect_worker(void *context) {
  race_worker *worker = context;
  orm_error_init(&worker->error);
  worker->status =
      orm_runtime_connect(worker->runtime, &worker->connect,
                          &worker->connection, &worker->error);
}

static void close_worker(void *context) {
  race_worker *worker = context;
  orm_error_init(&worker->error);
  worker->status = orm_runtime_close(worker->runtime, &worker->error);
}

static int load_control(race_control *control) {
  orm_error_t error;
  orm_error_init(&error);
  memset(control, 0, sizeof(*control));
  if (orm_module_open_absolute(fixture_path(), &control->module, &error) !=
      ORM_STATUS_OK)
    return 0;
  if (orm_module_symbol(&control->module, "orm_runtime_race_gate_arm",
                        &control->arm, sizeof(control->arm), &error) !=
          ORM_STATUS_OK ||
      orm_module_symbol(&control->module, "orm_runtime_race_gate_wait_entered",
                        &control->wait_entered,
                        sizeof(control->wait_entered), &error) !=
          ORM_STATUS_OK ||
      orm_module_symbol(&control->module, "orm_runtime_race_gate_release",
                        &control->release, sizeof(control->release), &error) !=
          ORM_STATUS_OK) {
    orm_module_close(&control->module);
    return 0;
  }
  return 1;
}

spec("runtime close and admission serialization") {
  it("keeps close BUSY while module initialization is outside the lock") {
    race_control control;
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    salts_thread_t worker_thread = NULL;
    race_worker worker;
    memset(&worker, 0, sizeof(worker));

    check_true(load_control(&control));
    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error), ORM_STATUS_OK);
    worker.runtime = runtime;
    worker.load = load_config();

    check_equal(control.arm(RACE_GATE_INITIALIZE), ORM_STATUS_OK);
    check_equal(salts_thread_create(&worker_thread, load_worker, &worker),
                0);
    check_true(control.wait_entered(RACE_TIMEOUT_MS));
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    control.release();
    check_equal(salts_thread_join(&worker_thread), 0);
    salts_thread_destroy(&worker_thread);
    check_equal(worker.status, ORM_STATUS_OK);

    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
    orm_module_close(&control.module);
  }

  it("keeps close BUSY while connection creation is outside the lock") {
    race_control control;
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    salts_thread_t worker_thread = NULL;
    race_worker worker;
    memset(&worker, 0, sizeof(worker));

    check_true(load_control(&control));
    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error), ORM_STATUS_OK);
    worker.runtime = runtime;
    worker.load = load_config();
    check_equal(orm_runtime_load_driver(runtime, &worker.load, &error),
                ORM_STATUS_OK);
    orm_config(&worker.connect);
    worker.connect.driver = orm_view("race");

    check_equal(control.arm(RACE_GATE_CONNECT), ORM_STATUS_OK);
    check_equal(salts_thread_create(&worker_thread, connect_worker, &worker),
                0);
    check_true(control.wait_entered(RACE_TIMEOUT_MS));
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    control.release();
    check_equal(salts_thread_join(&worker_thread), 0);
    salts_thread_destroy(&worker_thread);
    check_equal(worker.status, ORM_STATUS_OK);
    check_not_null(worker.connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);

    orm_disconnect(worker.connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
    orm_module_close(&control.module);
  }

  it("rejects new admission while module finalization is outside the lock") {
    race_control control;
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_info_t info;
    salts_thread_t worker_thread = NULL;
    race_worker worker;
    memset(&worker, 0, sizeof(worker));

    check_true(load_control(&control));
    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error), ORM_STATUS_OK);
    worker.runtime = runtime;
    worker.load = load_config();
    check_equal(orm_runtime_load_driver(runtime, &worker.load, &error),
                ORM_STATUS_OK);
    orm_config(&worker.connect);
    worker.connect.driver = orm_view("race");

    check_equal(control.arm(RACE_GATE_FINALIZE), ORM_STATUS_OK);
    check_equal(salts_thread_create(&worker_thread, close_worker, &worker),
                0);
    check_true(control.wait_entered(RACE_TIMEOUT_MS));

    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    check_equal(orm_runtime_driver_info(runtime, orm_view("race"),
                                        &info, &error),
                ORM_STATUS_INVALID_STATE);
    orm_connection_t *connection = NULL;
    check_equal(orm_runtime_connect(runtime, &worker.connect,
                                    &connection, &error),
                ORM_STATUS_INVALID_STATE);
    check_null(connection);
    check_equal(orm_runtime_load_driver(runtime, &worker.load, &error),
                ORM_STATUS_INVALID_STATE);

    control.release();
    check_equal(salts_thread_join(&worker_thread), 0);
    salts_thread_destroy(&worker_thread);
    check_equal(worker.status, ORM_STATUS_OK);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);

    orm_runtime_release(runtime);
    orm_module_close(&control.module);
  }
}
