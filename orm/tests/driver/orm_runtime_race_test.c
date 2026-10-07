#include <orm_runtime.h>

#include <salts/clock.h>
#include <salts/thread.h>
#include <tinytest.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RACE_TIMEOUT_MS UINT64_C(5000)
enum {
  RACE_GATE_INITIALIZE = 1u,
  RACE_GATE_CONNECT = 2u,
  RACE_GATE_FINALIZE = 3u
};

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

static const char *fixture_two_path(void) {
  const char *path = getenv("ORM_RUNTIME_RACE_FIXTURE_TWO");
  return path == NULL ? "" : path;
}

static orm_driver_load_config_t load_config_for(
    const char *path, const char *id) {
  orm_driver_load_config_t config;
  memset(&config, 0, sizeof(config));
  config.struct_size = (uint32_t)sizeof(config);
  config.abi_version = ORM_RUNTIME_ABI_VERSION;
  config.module_path = orm_view(path);
  config.expected_driver_id = orm_view(id);
  return config;
}

static orm_driver_load_config_t load_config(void) {
  return load_config_for(fixture_path(), "race");
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

static int race_marker_path(
    char *out, size_t out_size, const char *driver,
    uint32_t phase, const char *kind) {
  const char *prefix = getenv("ORM_RUNTIME_RACE_GATE_PREFIX");
  if (out == NULL || out_size == 0u || driver == NULL ||
      prefix == NULL || prefix[0] == '\0')
    return 0;
  const int written = snprintf(
      out, out_size, "%s-%s-%u-%s", prefix, driver,
      (unsigned)phase, kind);
  return written > 0 && (size_t)written < out_size;
}

static int race_marker_exists(const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) return 0;
  fclose(file);
  return 1;
}

static int race_marker_write(const char *path) {
  FILE *file = fopen(path, "wb");
  if (file == NULL) return 0;
  fputc('1', file);
  fclose(file);
  return 1;
}

static int race_arm(const char *driver, uint32_t phase) {
  char arm[1024], entered[1024], release[1024];
  if (!race_marker_path(arm, sizeof(arm), driver, phase, "arm") ||
      !race_marker_path(entered, sizeof(entered), driver, phase, "entered") ||
      !race_marker_path(release, sizeof(release), driver, phase, "release"))
    return 0;
  (void)remove(arm);
  (void)remove(entered);
  (void)remove(release);
  return race_marker_write(arm);
}

static int race_wait_entered(
    const char *driver, uint32_t phase, uint64_t timeout_ms) {
  char entered[1024];
  if (!race_marker_path(
          entered, sizeof(entered), driver, phase, "entered"))
    return 0;
  const uint64_t started = cmeta_monotonic_ms();
  while (cmeta_monotonic_ms() - started < timeout_ms) {
    if (race_marker_exists(entered)) return 1;
    cmeta_sleep_ms(1u);
  }
  return race_marker_exists(entered);
}

static int race_release(const char *driver, uint32_t phase) {
  char release[1024];
  return race_marker_path(
             release, sizeof(release), driver, phase, "release") &&
         race_marker_write(release);
}

spec("runtime close and admission serialization") {
  it("keeps close BUSY while Plugin start is outside the lock") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    cmeta_thread_t worker_thread = NULL;
    race_worker worker;
    memset(&worker, 0, sizeof(worker));

    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error), ORM_STATUS_OK);
    worker.runtime = runtime;
    worker.load = load_config();

    check_true(race_arm("race", RACE_GATE_INITIALIZE));
    check_equal(cmeta_thread_create(&worker_thread, load_worker, &worker),
                0);
    check_true(race_wait_entered("race", RACE_GATE_INITIALIZE, RACE_TIMEOUT_MS));
    orm_driver_info_t info;
    check_equal(orm_runtime_driver_info(runtime, orm_view("race"),
                                        &info, &error),
                ORM_STATUS_DRIVER_NOT_REGISTERED);
    check_equal(orm_runtime_load_driver(runtime, &worker.load, &error),
                ORM_STATUS_BUSY);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    check_true(race_release("race", RACE_GATE_INITIALIZE));
    check_equal(cmeta_thread_join(&worker_thread), 0);
    cmeta_thread_destroy(&worker_thread);
    check_equal(worker.status, ORM_STATUS_OK);

    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("keeps close BUSY while connection creation is outside the lock") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    cmeta_thread_t worker_thread = NULL;
    race_worker worker;
    memset(&worker, 0, sizeof(worker));

    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error), ORM_STATUS_OK);
    worker.runtime = runtime;
    worker.load = load_config();
    check_equal(orm_runtime_load_driver(runtime, &worker.load, &error),
                ORM_STATUS_OK);
    orm_config(&worker.connect);
    worker.connect.driver = orm_view("race");

    check_true(race_arm("race", RACE_GATE_CONNECT));
    check_equal(cmeta_thread_create(&worker_thread, connect_worker, &worker),
                0);
    check_true(race_wait_entered("race", RACE_GATE_CONNECT, RACE_TIMEOUT_MS));
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    check_true(race_release("race", RACE_GATE_CONNECT));
    check_equal(cmeta_thread_join(&worker_thread), 0);
    cmeta_thread_destroy(&worker_thread);
    check_equal(worker.status, ORM_STATUS_OK);
    check_not_null(worker.connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);

    orm_disconnect(worker.connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("loads two independent modules and multiple connections without cross-talk") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_info_t first_info, second_info;
    orm_connection_t *first = NULL, *second = NULL, *third = NULL;
    orm_config_t first_config, second_config;
    orm_driver_load_config_t first_load =
        load_config_for(fixture_path(), "race");
    orm_driver_load_config_t second_load =
        load_config_for(fixture_two_path(), "race2");

    check_true(fixture_path()[0] != '\0');
    check_true(fixture_two_path()[0] != '\0');
    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error), ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &first_load, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &second_load, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_driver_info(runtime, orm_view("race"),
                                        &first_info, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_driver_info(runtime, orm_view("race2"),
                                        &second_info, &error),
                ORM_STATUS_OK);
    check_equal(first_info.canonical_id_size, 4u);
    check_equal(second_info.canonical_id_size, 5u);

    orm_config(&first_config);
    first_config.driver = orm_view("race");
    orm_config(&second_config);
    second_config.driver = orm_view("race2");
    check_equal(orm_runtime_connect(runtime, &first_config, &first, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_connect(runtime, &first_config, &second, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_connect(runtime, &second_config, &third, &error),
                ORM_STATUS_OK);
    check_not_null(first);
    check_not_null(second);
    check_not_null(third);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);

    orm_disconnect(second);
    orm_disconnect(third);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    orm_disconnect(first);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("rolls back a driver factory failure without publishing a dependent") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_connection_t *connection = (orm_connection_t *)(uintptr_t)1u;
    orm_option_t option = {orm_view("fail_create"), orm_view("1")};
    orm_config_t connect;
    orm_driver_load_config_t load = load_config();

    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error), ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_OK);
    orm_config(&connect);
    connect.driver = orm_view("race");
    connect.options = &option;
    connect.option_count = 1u;
    check_equal(orm_runtime_connect(runtime, &connect, &connection, &error),
                ORM_STATUS_OUT_OF_MEMORY);
    check_null(connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("rejects new admission while Plugin stop is outside the lock") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_info_t info;
    cmeta_thread_t worker_thread = NULL;
    race_worker worker;
    memset(&worker, 0, sizeof(worker));

    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error), ORM_STATUS_OK);
    worker.runtime = runtime;
    worker.load = load_config();
    check_equal(orm_runtime_load_driver(runtime, &worker.load, &error),
                ORM_STATUS_OK);
    orm_config(&worker.connect);
    worker.connect.driver = orm_view("race");

    check_true(race_arm("race", RACE_GATE_FINALIZE));
    check_equal(cmeta_thread_create(&worker_thread, close_worker, &worker),
                0);
    check_true(race_wait_entered("race", RACE_GATE_FINALIZE, RACE_TIMEOUT_MS));

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

    check_true(race_release("race", RACE_GATE_FINALIZE));
    check_equal(cmeta_thread_join(&worker_thread), 0);
    cmeta_thread_destroy(&worker_thread);
    check_equal(worker.status, ORM_STATUS_OK);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);

    orm_runtime_release(runtime);
  }
}
