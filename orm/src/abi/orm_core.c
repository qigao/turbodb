#include "orm_internal.h"
#include "orm_command_publisher.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool orm_backend_valid(const orm_backend *backend) {
  return backend != NULL && backend->ops != NULL && backend->context != NULL &&
         backend->ops->struct_size >= sizeof(*backend->ops) &&
         backend->ops->abi_version == ORM_BACKEND_OPS_ABI_VERSION &&
         backend->ops->destroy != NULL && backend->ops->open_cursor != NULL &&
         backend->ops->execute_command != NULL &&
         backend->ops->begin_transaction != NULL;
}

static bool orm_transaction_backend_valid(
    const orm_transaction_backend *backend) {
  return backend != NULL && backend->ops != NULL && backend->context != NULL &&
         backend->ops->struct_size >= sizeof(*backend->ops) &&
         backend->ops->abi_version == ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION &&
         backend->ops->destroy != NULL && backend->ops->open_cursor != NULL &&
         backend->ops->execute_command != NULL && backend->ops->commit != NULL &&
         backend->ops->rollback != NULL;
}

void orm_error_set(orm_error_t *error, orm_status_t status,
                   const char *message) {
  if (error == NULL || error->struct_size < sizeof(*error))
    return;
  error->status = status;
  (void)snprintf(error->message, sizeof(error->message), "%s",
                 status == ORM_STATUS_OK
                     ? ""
                     : (message != NULL ? message
                                        : orm_status_message(status)));
}


#if defined(ORM_NATIVE_OWNER_CANDIDATE)
/* This candidate is compiled only into the non-installed native test core.
 * Resource cleanup runs after the control lock is released. */
/* Failure is terminal for business operations, but never consumes a hold or
 * closes resources. A query/Pub can outlive its application's connection ref. */
static orm_status_t orm_connection_business_status(orm_connection_t *connection,
                                                    orm_error_t *error) {
  orm_status_t status;
  orm_status_t cause;
  if (connection == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  salts_mutex_lock(&connection->owner.mutex);
  cause = connection->failure;
  status = cause == ORM_STATUS_OK ? ORM_STATUS_OK : ORM_STATUS_INVALID_STATE;
  salts_mutex_unlock(&connection->owner.mutex);
  if (status != ORM_STATUS_OK)
    orm_error_set(error, status, cause == ORM_OWNER_STATUS_CLEANUP_FAILED
        ? "connection is unusable after a cleanup failure"
        : cause == ORM_STATUS_CONNECTION_ERROR
            ? "connection is unusable after native connection loss"
            : "connection is unusable after an uncertain commit");
  return status;
}

/* Native error classification belongs to the backend. The host records only
 * terminal connection failure; it neither destroys the session nor guesses
 * whether a dispatched write committed. Existing stronger failures win. */
static void orm_connection_record_native_error(orm_connection_t *connection,
                                                orm_status_t status) {
  if (status != ORM_STATUS_CONNECTION_ERROR) return;
  salts_mutex_lock(&connection->owner.mutex);
  if (connection->failure == ORM_STATUS_OK)
    connection->failure = status;
  salts_mutex_unlock(&connection->owner.mutex);
}

/* The Publisher's query hold keeps this context and its parent valid. These
 * hooks allocate/release nothing and run without a native or owner lock. */
static orm_status_t orm_query_execution_status(void *context, orm_error_t *error) {
  const orm_query_t *query = context;
  return orm_connection_business_status(query->connection, error);
}

static void orm_query_report_native_error(void *context, orm_status_t status) {
  const orm_query_t *query = context;
  orm_connection_record_native_error(query->connection, status);
}

static void orm_connection_mark_unknown(orm_connection_t *connection) {
  salts_mutex_lock(&connection->owner.mutex);
  connection->failure = ORM_OWNER_STATUS_COMMIT_UNKNOWN;
  salts_mutex_unlock(&connection->owner.mutex);
}

static orm_status_t orm_connection_admit_business(orm_connection_t *connection) {
  orm_owner *owner = &connection->owner;
  orm_status_t status = ORM_STATUS_OK;
  salts_mutex_lock(&owner->mutex);
  if (connection->failure != ORM_STATUS_OK || owner->phase != ORM_OWNER_OPEN ||
      owner->references == 0u)
    status = ORM_STATUS_INVALID_STATE;
  else if (owner->dependents >= owner->max_dependents)
    status = ORM_STATUS_LIMIT_EXCEEDED;
  else
    ++owner->dependents;
  salts_mutex_unlock(&owner->mutex);
  return status;
}

static void orm_connection_action(orm_connection_t *connection,
                                  orm_owner_action action) {
  if (action == ORM_OWNER_KEEP) return;
  salts_mutex_lock(&connection->owner.mutex);
  if (connection->native_active) {
    /* Final cleanup can consume the last inherited parent hold. The exclusive
     * CLOSING/CLOSED action keeps this allocation alive until the lane exits. */
    if (connection->native_final_action != ORM_OWNER_KEEP) abort();
    connection->native_final_action = action;
    salts_mutex_unlock(&connection->owner.mutex);
    return;
  }
  salts_mutex_unlock(&connection->owner.mutex);
  if (action == ORM_OWNER_CLOSE_RESOURCES) {
    if (connection->backend.ops != NULL && connection->backend.context != NULL)
      connection->backend.ops->destroy(connection->backend.context);
    memset(&connection->backend, 0, sizeof(connection->backend));
    action = orm_owner_finish_close(&connection->owner);
  }
  if (action == ORM_OWNER_FREE_MEMORY) {
    orm_owner_dispose(&connection->owner);
    free(connection);
  }
}

static void orm_connection_release_child(orm_connection_t *connection) {
  orm_connection_action(connection, orm_owner_release_dependent(&connection->owner));
}

/* The active native caller is the sole drainer. Keep the lane occupied through
 * run AND finish: finishing a cursor can enqueue its transaction finalizer.
 * Each node has at most two coalesced operations. Queue storage is bounded by
 * existing query/transaction/Publisher admission budgets, not allocated here. */
static void orm_connection_drain_cleanup(orm_connection_t *connection) {
  for (;;) {
    salts_mutex_lock(&connection->owner.mutex);
    if (!connection->native_active) abort();
    orm_native_cleanup *node = connection->cleanup_head;
    if (node == NULL) {
      const orm_owner_action final_action = connection->native_final_action;
      connection->native_final_action = ORM_OWNER_KEEP;
      connection->native_active = false;
      salts_mutex_unlock(&connection->owner.mutex);
      orm_connection_action(connection, final_action); /* May free connection. */
      return;
    }
    const unsigned pending = node->requested & ~node->completed;
    void *context = node->context;
    if (pending != 0u) {
      void (*run)(void *, unsigned) = node->run;
      node->completed |= pending;
      salts_mutex_unlock(&connection->owner.mutex);
      run(context, pending);
      /* Still linked and live. Reentrant requests may have added DESTROY to
       * a CANCEL in progress; consume those before unqueuing or freeing it. */
      continue;
    }
    const unsigned completed = node->completed;
    void (*finish)(void *, unsigned) = node->finish;
    connection->cleanup_head = node->next;
    if (connection->cleanup_head == NULL) connection->cleanup_tail = NULL;
    node->next = NULL;
    node->queued = false;
    node->requested = node->completed = 0u;
    salts_mutex_unlock(&connection->owner.mutex);
    finish(context, completed); /* May free node and release every parent hold. */
  }
}

static void orm_connection_request_cleanup(orm_connection_t *connection,
                                            orm_native_cleanup *node,
                                            unsigned request) {
  if (node == NULL || node->run == NULL || node->finish == NULL ||
      request == 0u ||
      (request & ~(ORM_NATIVE_CLEANUP_CANCEL | ORM_NATIVE_CLEANUP_DESTROY)) != 0u)
    abort();
  salts_mutex_lock(&connection->owner.mutex);
  node->requested |= request;
  if (!node->queued) {
    node->queued = true;
    node->next = NULL;
    if (connection->cleanup_tail != NULL)
      connection->cleanup_tail->next = node;
    else
      connection->cleanup_head = node;
    connection->cleanup_tail = node;
  }
  const bool drain = !connection->native_active;
  if (drain) connection->native_active = true;
  salts_mutex_unlock(&connection->owner.mutex);
  /* Cleanup is legal after business failure and at a full dependent budget.
   * Its already admitted object holds survive until finish, and native_active
   * defers the final connection action if finishing consumes the last hold. */
  if (drain) orm_connection_drain_cleanup(connection);
}

/* Derive a bounded native-call hold from an existing query/transaction/creation
 * hold. RELEASE_PENDING is legal here: no new external handle is admitted.
 * Commands, controls, construction, row resumes and native cleanup share this
 * slot. No mutex is held across a native callback or cleanup completion. */
static orm_status_t orm_connection_begin_native(orm_connection_t *connection,
                                                  orm_error_t *error) {
  orm_owner *owner = &connection->owner;
  orm_status_t status = ORM_STATUS_OK;
  salts_mutex_lock(&owner->mutex);
  if (connection->failure != ORM_STATUS_OK ||
      (owner->phase != ORM_OWNER_OPEN && owner->phase != ORM_OWNER_RELEASE_PENDING))
    status = ORM_STATUS_INVALID_STATE;
  else if (connection->native_active)
    status = ORM_STATUS_BUSY;
  else if (owner->dependents >= owner->max_dependents)
    status = ORM_STATUS_LIMIT_EXCEEDED;
  else {
    connection->native_active = true;
    ++owner->dependents;
  }
  salts_mutex_unlock(&owner->mutex);
  if (status == ORM_STATUS_INVALID_STATE &&
      orm_connection_business_status(connection, error) != ORM_STATUS_OK)
    return status; /* Preserve the existing terminal-failure diagnostic. */
  orm_error_set(error, status, status == ORM_STATUS_BUSY
      ? "connection native operation is busy" : NULL);
  return status;
}

static void orm_connection_end_native(orm_connection_t *connection,
                                        orm_status_t status) {
  salts_mutex_lock(&connection->owner.mutex);
  if (!connection->native_active) abort();
  /* Record failure before making the slot available to another native call. */
  if (status == ORM_STATUS_CONNECTION_ERROR && connection->failure == ORM_STATUS_OK)
    connection->failure = status;
  salts_mutex_unlock(&connection->owner.mutex);
  /* The admitted completion hold keeps the parent alive throughout the drain. */
  orm_connection_drain_cleanup(connection);
  orm_connection_release_child(connection);
}

/* The Publisher keeps its query hold; only the bounded connection completion
 * hold is acquired/released here. End follows the full reader decode, not next.
 * Native failures have already been recorded by report_owner_error. */
static orm_status_t orm_query_begin_row_execution(void *context,
                                                    orm_error_t *error) {
  const orm_query_t *query = context;
  return orm_connection_begin_native(query->connection, error);
}

static void orm_query_end_row_execution(void *context) {
  const orm_query_t *query = context;
  orm_connection_end_native(query->connection, ORM_STATUS_OK);
}

static void orm_query_request_row_cleanup(void *context,
                                           orm_native_cleanup *node,
                                           unsigned request) {
  const orm_query_t *query = context;
  orm_connection_request_cleanup(query->connection, node, request);
}

/* Final release has no synchronous error receiver. The default policy cannot
 * silently continue; an explicit host handler may return, but cannot recover
 * or unload the quarantined native state. */
static void orm_cleanup_fail_fast(const orm_error_t *native_error) {
  (void)fprintf(stderr, "ORM cleanup failed (%d): %s\n",
                 (int)native_error->status, native_error->message);
  (void)fflush(stderr);
  abort();
}

static void orm_transaction_quarantine(orm_transaction_t *transaction,
                                       const orm_error_t *native_error) {
  orm_connection_t *connection = transaction->connection;
  orm_owner_cleanup_policy policy;
  /* Lock order matches admission: parent before child. Neither native cleanup
   * nor the host error handler executes while either control lock is held. */
  salts_mutex_lock(&connection->owner.mutex);
  salts_mutex_lock(&transaction->owner.mutex);
  if (transaction->owner.phase != ORM_OWNER_CLOSING ||
      transaction->owner.dependents != 0u) abort();
  transaction->cleanup_error = *native_error;
  transaction->owner.phase = ORM_OWNER_CLOSE_FAILED;
  if (connection->failure != ORM_OWNER_STATUS_CLEANUP_FAILED)
    connection->cleanup_error = *native_error;
  connection->failure = ORM_OWNER_STATUS_CLEANUP_FAILED;
  connection->owner.phase = ORM_OWNER_CLOSE_FAILED;
  policy = connection->cleanup_policy;
  salts_mutex_unlock(&transaction->owner.mutex);
  salts_mutex_unlock(&connection->owner.mutex);
  if (policy.notify != NULL)
    policy.notify(policy.context, &transaction->cleanup_error);
  else
    orm_cleanup_fail_fast(&transaction->cleanup_error);
}

static void orm_transaction_action(orm_transaction_t *, orm_owner_action);

/* CLOSING preserves transaction memory while native cleanup is queued/running.
 * A failed rollback still quarantines BEFORE destroy or parent-hold release. */
static void orm_transaction_run_cleanup(void *context, unsigned request) {
  orm_transaction_t *transaction = context;
  (void)request;
  if (transaction->state == ORM_TRANSACTION_ACTIVE &&
      orm_transaction_backend_valid(&transaction->backend)) {
    orm_error_t cleanup_error;
    orm_error_init(&cleanup_error);
    const orm_status_t status = transaction->backend.ops->rollback(
        transaction->backend.context, &cleanup_error);
    if (status != ORM_STATUS_OK) {
      /* The returned status is authoritative even when a backend leaves the
       * optional error buffer unfilled. The host owns and bounds this copy. */
      cleanup_error.struct_size = sizeof(cleanup_error);
      cleanup_error.status = status;
      cleanup_error.message[sizeof(cleanup_error.message) - 1u] = '\0';
      if (cleanup_error.message[0] == '\0')
        orm_error_set(&cleanup_error, status, NULL);
      orm_transaction_quarantine(transaction, &cleanup_error);
      return;
    }
  }
  if (transaction->backend.ops != NULL &&
      transaction->backend.ops->destroy != NULL &&
      transaction->backend.context != NULL)
    transaction->backend.ops->destroy(transaction->backend.context);
  memset(&transaction->backend, 0, sizeof(transaction->backend));
}

static void orm_transaction_finish_cleanup(void *context, unsigned completed) {
  orm_transaction_t *transaction = context;
  (void)completed;
  salts_mutex_lock(&transaction->owner.mutex);
  const bool failed = transaction->owner.phase == ORM_OWNER_CLOSE_FAILED;
  salts_mutex_unlock(&transaction->owner.mutex);
  if (!failed)
    orm_transaction_action(transaction, orm_owner_finish_close(&transaction->owner));
}

static void orm_transaction_action(orm_transaction_t *transaction,
                                   orm_owner_action action) {
  if (action == ORM_OWNER_CLOSE_RESOURCES) {
    if (transaction->backend.ops != NULL && transaction->backend.context != NULL) {
      orm_native_cleanup *node = &transaction->native_cleanup;
      node->run = orm_transaction_run_cleanup;
      node->finish = orm_transaction_finish_cleanup;
      node->context = transaction;
      orm_connection_request_cleanup(transaction->connection, node,
                                      ORM_NATIVE_CLEANUP_DESTROY);
      return; /* May already have completed and freed transaction. */
    }
    /* A refused/failed BEGIN owns no native state and needs no deferred work. */
    action = orm_owner_finish_close(&transaction->owner);
  }
  if (action == ORM_OWNER_FREE_MEMORY) {
    orm_connection_t *connection = transaction->connection;
    orm_owner_dispose(&transaction->owner);
    free(transaction);
    orm_connection_release_child(connection);
  }
}

/* Native work and close admission share the owner lock. No native callback
 * runs under it; CLOSING protects even a last-reference release in destroy. */
orm_status_t ORM_C_CALL orm_transaction_close(orm_transaction_t *transaction,
                                              orm_error_t *error) {
  orm_status_t status = ORM_STATUS_OK;
  orm_owner_action action = ORM_OWNER_KEEP;
  if (transaction == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "transaction handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  orm_owner *owner = &transaction->owner;
  orm_connection_t *connection = transaction->connection;
  salts_mutex_lock(&connection->owner.mutex);
  salts_mutex_lock(&owner->mutex);
  if (owner->phase == ORM_OWNER_CLOSE_FAILED) {
    status = ORM_OWNER_STATUS_CLEANUP_FAILED;
  } else if (owner->phase == ORM_OWNER_CLOSED) {
    /* A closed handle can still be held; close does not consume its reference. */
  } else if (owner->dependents != 0u || transaction->operation_active ||
             owner->phase == ORM_OWNER_CLOSING || connection->native_active) {
    status = ORM_STATUS_BUSY;
  } else if (owner->phase != ORM_OWNER_OPEN || owner->references == 0u ||
             (transaction->state != ORM_TRANSACTION_COMMITTED &&
              transaction->state != ORM_TRANSACTION_ROLLED_BACK &&
              transaction->state != ORM_TRANSACTION_COMMIT_UNKNOWN)) {
    status = ORM_STATUS_INVALID_STATE;
  } else {
    owner->phase = ORM_OWNER_CLOSING;
    action = ORM_OWNER_CLOSE_RESOURCES;
    connection->native_active = true; /* Reserve close before releasing either lock. */
  }
  salts_mutex_unlock(&owner->mutex);
  salts_mutex_unlock(&connection->owner.mutex);
  if (action == ORM_OWNER_CLOSE_RESOURCES) {
    orm_transaction_action(transaction, action); /* Enqueues under our reservation. */
    orm_connection_drain_cleanup(connection);
  }
  orm_error_set(error, status, status == ORM_OWNER_STATUS_CLEANUP_FAILED
      ? transaction->cleanup_error.message : NULL);
  return status;
}

void ORM_C_CALL orm_transaction_retain(orm_transaction_t *transaction) {
  if (transaction == NULL ||
      orm_owner_try_retain(&transaction->owner) != ORM_STATUS_OK) abort();
}

void ORM_C_CALL orm_transaction_release(orm_transaction_t *transaction) {
  if (transaction != NULL)
    orm_transaction_action(transaction,
                           orm_owner_release_reference(&transaction->owner));
}

static void orm_transaction_release_execution(void *context) {
  orm_transaction_t *transaction = context;
  orm_transaction_action(transaction,
                         orm_owner_release_dependent(&transaction->owner));
}

static orm_status_t orm_transaction_admit(orm_transaction_t *transaction,
                                          orm_error_t *error) {
  orm_owner *owner = &transaction->owner;
  orm_status_t status = ORM_STATUS_OK;
  salts_mutex_lock(&owner->mutex);
  if (transaction->operation_active)
    status = ORM_STATUS_BUSY;
  else if (owner->phase != ORM_OWNER_OPEN || owner->references == 0u ||
           transaction->state != ORM_TRANSACTION_ACTIVE)
    status = ORM_STATUS_INVALID_STATE;
  else if (owner->dependents >= owner->max_dependents)
    status = ORM_STATUS_LIMIT_EXCEEDED;
  else
    ++owner->dependents;
  salts_mutex_unlock(&owner->mutex);
  orm_error_set(error, status, NULL);
  return status;
}

static orm_status_t orm_transaction_begin_operation(
    orm_transaction_t *transaction, orm_error_t *error) {
  orm_status_t status;
  if (transaction == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE, "ORM transaction is not active");
    return ORM_STATUS_INVALID_STATE;
  }
  orm_connection_t *connection = transaction->connection;
  status = orm_connection_begin_native(connection, error);
  if (status != ORM_STATUS_OK) return status;
  status = orm_owner_begin_write(&transaction->owner);
  if (status != ORM_STATUS_OK) {
    orm_connection_end_native(connection, ORM_STATUS_OK);
    orm_error_set(error, status, "transaction control operation is not admitted");
    return status;
  }
  if (transaction->state != ORM_TRANSACTION_ACTIVE ||
      !orm_transaction_backend_valid(&transaction->backend)) {
    orm_owner_end_write(&transaction->owner);
    orm_connection_end_native(connection, ORM_STATUS_OK);
    orm_error_set(error, ORM_STATUS_INVALID_STATE, "ORM transaction is not active");
    return ORM_STATUS_INVALID_STATE;
  }
  /* begin_write established zero dependents and a nonzero configured budget.
   * Reserve completion before unlocking: a callback may release the last public
   * reference, but cannot cause cleanup while it is still executing. */
  transaction->operation_active = true;
  transaction->owner.dependents = 1u;
  orm_owner_end_write(&transaction->owner);
  return ORM_STATUS_OK;
}

static void orm_transaction_end_operation(orm_transaction_t *transaction,
                                          orm_status_t status,
                                          orm_transaction_state next_state) {
  /* Completion may release the last transaction reference and free it. The
   * connection reservation keeps the cached parent live through that cleanup. */
  orm_connection_t *connection = transaction->connection;
  if (status == ORM_OWNER_STATUS_COMMIT_UNKNOWN)
    orm_connection_mark_unknown(connection);
  else
    orm_connection_record_native_error(connection, status);
  salts_mutex_lock(&transaction->owner.mutex);
  if (status == ORM_OWNER_STATUS_COMMIT_UNKNOWN)
    transaction->state = ORM_TRANSACTION_COMMIT_UNKNOWN;
  else if (status == ORM_STATUS_OK)
    transaction->state = next_state;
  transaction->operation_active = false;
  salts_mutex_unlock(&transaction->owner.mutex);
  orm_transaction_release_execution(transaction);
  /* Publish the final state/error and finish callback-triggered cleanup before
   * admitting another command or control operation on this connection. */
  orm_connection_end_native(connection, status);
}

static void orm_query_action(orm_query_t *query, orm_owner_action action) {
  if (action == ORM_OWNER_CLOSE_RESOURCES) {
    orm_plan_destroy(&query->plan);
    action = orm_owner_finish_close(&query->owner);
  }
  if (action == ORM_OWNER_FREE_MEMORY) {
    orm_connection_t *connection = query->connection;
    orm_owner_dispose(&query->owner);
    free(query);
    orm_connection_release_child(connection);
  }
}

static void orm_query_release_execution(void *context) {
  orm_query_t *query = context;
  orm_query_action(query, orm_owner_release_dependent(&query->owner));
}

orm_status_t ORM_C_CALL orm_connection_close(orm_connection_t *connection,
                                             orm_error_t *error) {
  orm_owner_action action = ORM_OWNER_KEEP;
  orm_status_t status = connection != NULL
      ? orm_owner_begin_close(&connection->owner, &action)
      : ORM_STATUS_INVALID_ARGUMENT;
  if (status == ORM_STATUS_OK) orm_connection_action(connection, action);
  orm_error_set(error, status, status == ORM_OWNER_STATUS_CLEANUP_FAILED
      ? connection->cleanup_error.message : NULL);
  return status;
}

void ORM_C_CALL orm_connection_retain(orm_connection_t *connection) {
  if (connection == NULL || orm_owner_try_retain(&connection->owner) != ORM_STATUS_OK)
    abort();
}

void ORM_C_CALL orm_connection_release(orm_connection_t *connection) {
  if (connection != NULL)
    orm_connection_action(connection, orm_owner_release_reference(&connection->owner));
}

orm_status_t ORM_C_CALL orm_query_close(orm_query_t *query, orm_error_t *error) {
  orm_owner_action action = ORM_OWNER_KEEP;
  orm_status_t status = query != NULL
      ? orm_owner_begin_close(&query->owner, &action) : ORM_STATUS_INVALID_ARGUMENT;
  if (status == ORM_STATUS_OK) orm_query_action(query, action);
  orm_error_set(error, status, NULL);
  return status;
}

void ORM_C_CALL orm_query_retain(orm_query_t *query) {
  if (query == NULL || orm_owner_try_retain(&query->owner) != ORM_STATUS_OK) abort();
}

void ORM_C_CALL orm_query_release(orm_query_t *query) {
  if (query != NULL)
    orm_query_action(query, orm_owner_release_reference(&query->owner));
}
#endif

bool orm_view_valid(vstr value, bool allow_empty) {
  return (allow_empty || value.len != 0u) &&
         (value.len == 0u || value.data != NULL);
}

bool orm_view_equal_cstr(vstr value, const char *text) {
  size_t length;
  if (text == NULL || !orm_view_valid(value, true))
    return false;
  length = strlen(text);
  return value.len == length &&
         (length == 0u || memcmp(value.data, text, length) == 0);
}

const orm_option_t *orm_option_find(const orm_config_t *config,
                                    const char *keyword) {
  uint32_t index;
  if (config == NULL || keyword == NULL)
    return NULL;
  for (index = 0u; index < config->option_count; ++index) {
    if (orm_view_equal_cstr(config->options[index].keyword, keyword))
      return &config->options[index];
  }
  return NULL;
}

static bool orm_u64_to_size(uint64_t input, size_t *output) {
  if (output == NULL || input > (uint64_t)SIZE_MAX)
    return false;
  *output = (size_t)input;
  return true;
}

static orm_status_t orm_limits_from_config(const orm_config_t *config,
                                           orm_limits *limits,
                                           orm_error_t *error) {
  if (config == NULL || limits == NULL ||
      config->struct_size != sizeof(*config) ||
      config->abi_version != ORM_C_ABI_VERSION) {
    orm_error_set(error, ORM_STATUS_ABI_MISMATCH,
                  "ORM configuration ABI does not match this build");
    return ORM_STATUS_ABI_MISMATCH;
  }
  if (!orm_view_valid(config->driver, false) ||
      (config->option_count != 0u && config->options == NULL) ||
      config->max_parameters == 0u || config->max_columns == 0u ||
      config->max_predicates == 0u || config->max_assignments == 0u ||
      config->max_query_bytes == 0u || config->max_parameter_bytes == 0u ||
      config->max_result_rows == 0u || config->max_result_bytes == 0u ||
      !orm_u64_to_size(config->max_query_bytes, &limits->max_query_bytes) ||
      !orm_u64_to_size(config->max_parameter_bytes,
                       &limits->max_parameter_bytes)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid ORM configuration limits");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  limits->max_parameters = config->max_parameters;
  limits->max_columns = config->max_columns;
  limits->max_predicates = config->max_predicates;
  limits->max_assignments = config->max_assignments;
  limits->max_result_rows = config->max_result_rows;
  limits->max_result_bytes = config->max_result_bytes;
  return ORM_STATUS_OK;
}

static orm_status_t orm_backend_create(const orm_config_t *config,
                                       const orm_limits *limits,
                                       orm_backend *backend,
                                       orm_error_t *error) {
  orm_status_t status = ORM_STATUS_UNSUPPORTED;
  (void)config;
  (void)limits;
  memset(backend, 0, sizeof(*backend));
#if defined(ORM_WITH_SQLITE)
  if (orm_view_equal_cstr(config->driver, "sqlite"))
    status = orm_sqlite_backend_create(config, limits, backend, error);
  else
#endif
#if defined(ORM_WITH_REDIS)
  if (orm_view_equal_cstr(config->driver, "redis"))
    status = orm_redis_backend_create(config, limits, backend, error);
  else
#endif
#if defined(ORM_WITH_TIDESDB)
  if (orm_view_equal_cstr(config->driver, "tidesdb"))
    status = orm_tidesdb_backend_create(config, limits, backend, error);
  else
#endif
#if defined(ORM_WITH_MONGO)
  if (orm_view_equal_cstr(config->driver, "mongo") ||
      orm_view_equal_cstr(config->driver, "mongodb"))
    status = orm_mongo_backend_create(config, limits, backend, error);
  else
#endif
  {
    orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                  "requested ORM driver is not enabled");
    status = ORM_STATUS_UNSUPPORTED;
  }
  return status;
}

static bool orm_valid_isolation(orm_isolation_t isolation) {
  return isolation >= ORM_ISOLATION_READ_UNCOMMITTED &&
         isolation <= ORM_ISOLATION_SERIALIZABLE;
}

static bool orm_sql_has_keyword(const unsigned char *sql, size_t size,
                                const char *keyword) {
  size_t index = 0u;
  const size_t keyword_size = strlen(keyword);
  while (index < size) {
    if (sql[index] == '\'' || sql[index] == '"') {
      const unsigned char quote = sql[index++];
      while (index < size) {
        if (sql[index++] != quote)
          continue;
        if (index < size && sql[index] == quote) {
          ++index;
          continue;
        }
        break;
      }
      continue;
    }
    if (index + 1u < size && sql[index] == '-' && sql[index + 1u] == '-') {
      index += 2u;
      while (index < size && sql[index] != '\n')
        ++index;
      continue;
    }
    if (index + 1u < size && sql[index] == '/' && sql[index + 1u] == '*') {
      index += 2u;
      while (index + 1u < size &&
             !(sql[index] == '*' && sql[index + 1u] == '/'))
        ++index;
      index = index + 1u < size ? index + 2u : size;
      continue;
    }
    if ((index == 0u || !(isalnum(sql[index - 1u]) || sql[index - 1u] == '_')) &&
        index + keyword_size <= size) {
      size_t offset = 0u;
      while (offset < keyword_size &&
             tolower(sql[index + offset]) == (unsigned char)keyword[offset])
        ++offset;
      if (offset == keyword_size &&
          (index + offset == size ||
           !(isalnum(sql[index + offset]) || sql[index + offset] == '_')))
        return true;
    }
    ++index;
  }
  return false;
}

bool orm_query_returns_rows(const orm_query_plan *plan) {
  const unsigned char *cursor;
  size_t remaining;
  char keyword[8];
  size_t size = 0u;
  if (plan == NULL || plan->raw_sql == NULL)
    return false;
  cursor = (const unsigned char *)plan->raw_sql;
  remaining = tstr_len(plan->raw_sql);
  while (remaining != 0u && isspace(*cursor)) {
    ++cursor;
    --remaining;
  }
  while (remaining != 0u && size + 1u < sizeof(keyword) &&
         isalpha(*cursor)) {
    keyword[size++] = (char)tolower(*cursor);
    ++cursor;
    --remaining;
  }
  keyword[size] = '\0';
  return strcmp(keyword, "select") == 0 || strcmp(keyword, "with") == 0 ||
         strcmp(keyword, "pragma") == 0 || strcmp(keyword, "explain") == 0 ||
         orm_sql_has_keyword((const unsigned char *)plan->raw_sql,
                             tstr_len(plan->raw_sql), "returning");
}

static orm_status_t orm_query_make(orm_connection_t *connection, vstr input,
                                   orm_query_kind kind,
                                   orm_query_t **out_query,
                                   orm_error_t *error) {
  orm_query_t *query;
  orm_status_t status;
  if (out_query != NULL) *out_query = NULL;
  if (connection == NULL || out_query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "invalid ORM query creation arguments");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  status = orm_connection_admit_business(connection);
  if (status != ORM_STATUS_OK) {
    orm_error_set(error, status, "connection cannot admit a query");
    return status;
  }
#endif
  if (!orm_backend_valid(&connection->backend)) {
    status = ORM_STATUS_INVALID_ARGUMENT;
    orm_error_set(error, status, "invalid ORM query creation arguments");
    goto release_parent;
  }
  query = (orm_query_t *)calloc(1u, sizeof(*query));
  if (query == NULL) {
    status = ORM_STATUS_OUT_OF_MEMORY;
    orm_error_set(error, status, "allocate ORM query");
    goto release_parent;
  }
  query->connection = connection;
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  status = orm_owner_init(&query->owner, ORM_OWNER_DEFAULT_REFERENCES,
                          ORM_OWNER_DEFAULT_DEPENDENTS);
  if (status != ORM_STATUS_OK) {
    free(query);
    orm_error_set(error, status, "initialize query owner");
    goto release_parent;
  }
#endif
  status = orm_plan_init(&query->plan, kind, input, &connection->limits, error);
  if (status != ORM_STATUS_OK) {
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
    orm_query_release(query);
    return status;
#else
    free(query);
    goto release_parent;
#endif
  }
  *out_query = query;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
release_parent:
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_connection_release_child(connection);
#endif
  return status;
}

typedef struct orm_lazy_command_state {
  orm_query_t *query;
  orm_backend *database;
  orm_transaction_t *transaction;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
} orm_lazy_command_state;

static orm_command_driver_result orm_lazy_command_execute(void *context) {
  orm_lazy_command_state *state = (orm_lazy_command_state *)context;
  orm_command_driver_result output = ORM_COMMAND_DRIVER_RESULT_INIT;
  orm_error_t error;
  uint64_t affected = 0u;
  orm_status_t status;
  orm_error_init(&error);
  if (state == NULL || state->query == NULL) {
    output.status = ORM_STATUS_INVALID_STATE;
    output.message = "invalid lazy ORM command state";
    return output;
  }
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  status = orm_connection_begin_native(state->query->connection, &error);
  if (status != ORM_STATUS_OK) {
    output.status = status;
    (void)snprintf(state->error_message, sizeof(state->error_message), "%s", error.message);
    output.message = state->error_message;
    return output;
  }
#endif
  if (state->database != NULL) {
    status = state->database->ops->execute_command(
        state->database->context, &state->query->plan,
        &state->query->connection->limits, &affected, &error);
  } else {
    status = state->transaction->backend.ops->execute_command(
        state->transaction->backend.context, &state->query->plan,
        &state->query->connection->limits, &affected, &error);
  }
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_connection_end_native(state->query->connection, status);
#endif
  output.status = status;
  output.affected_rows = affected;
  if (status != ORM_STATUS_OK) {
    (void)snprintf(state->error_message, sizeof(state->error_message), "%s",
                   error.message[0] != '\0' ? error.message
                                             : orm_status_message(status));
    output.message = state->error_message;
  }
  return output;
}

static void orm_lazy_command_destroy(void *context) {
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_lazy_command_state *state = context;
  orm_query_t *query = state->query;
  orm_transaction_t *transaction = state->transaction;
#endif
  free(context);
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  if (transaction != NULL) orm_transaction_release_execution(transaction);
  orm_query_release_execution(query);
#endif
}

static const orm_command_driver_ops orm_lazy_command_ops = {
    sizeof(orm_command_driver_ops), ORM_COMMAND_DRIVER_OPS_ABI_VERSION,
    orm_lazy_command_execute, orm_lazy_command_destroy};

static orm_status_t orm_open_command(orm_query_t *query,
                                     orm_backend *database,
                                     orm_transaction_t *transaction,
                                     cflow_publisher *out_publisher,
                                     orm_error_t *error) {
  orm_lazy_command_state *state;
  orm_command_driver driver;
  orm_status_t status;
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  bool transaction_held = false;
#endif
  if (query == NULL || out_publisher == NULL || cflow_publisher_valid(out_publisher)) {
    status = out_publisher != NULL && cflow_publisher_valid(out_publisher)
                 ? ORM_STATUS_INVALID_STATE : ORM_STATUS_INVALID_ARGUMENT;
    orm_error_set(error, status, "invalid ORM command Publisher open");
    return status;
  }
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  status = orm_owner_admit(&query->owner);
  if (status != ORM_STATUS_OK) {
    orm_error_set(error, status, "query cannot admit a Publisher");
    return status;
  }
  status = orm_connection_business_status(query->connection, error);
  if (status != ORM_STATUS_OK) goto release_query;
  if (transaction != NULL) {
    status = orm_transaction_admit(transaction, error);
    if (status != ORM_STATUS_OK) goto release_query;
    transaction_held = true;
  }
#endif
  if (query->connection == NULL || query->plan.kind == ORM_QUERY_SELECT ||
      (query->plan.kind == ORM_QUERY_RAW && orm_query_returns_rows(&query->plan))) {
    status = ORM_STATUS_INVALID_ARGUMENT;
    orm_error_set(error, status, "invalid ORM command Publisher open");
    goto release_query;
  }
  state = (orm_lazy_command_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    status = ORM_STATUS_OUT_OF_MEMORY;
    orm_error_set(error, status, "allocate lazy ORM command");
    goto release_query;
  }
  state->query = query;
  state->database = database;
  state->transaction = transaction;
  driver.ops = &orm_lazy_command_ops;
  driver.context = state;
  status = orm_command_publisher_init(out_publisher, &driver, error);
  if (status == ORM_STATUS_OK) return status;
  free(state);
release_query:
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  if (transaction_held) orm_transaction_release_execution(transaction);
  orm_query_release_execution(query);
#endif
  return status;
}

static orm_status_t orm_open_rows(orm_query_t *query, orm_backend *database,
                                  orm_transaction_t *transaction,
                                  const orm_flow_config_t *config,
                                  cflow_publisher *out_publisher,
                                  orm_error_t *error) {
  orm_row_cursor cursor = {0};
  orm_row_publisher_config publisher_config;
  orm_row_publisher_prepared *prepared = NULL;
  cflow_publisher timed_publisher = {0};
  uint64_t wait_timeout_ns;
  orm_status_t status;
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  bool transaction_held = false;
  bool native_held = false;
  orm_connection_t *connection = NULL;
#endif
  if (query == NULL || query->connection == NULL || config == NULL ||
      config->struct_size != sizeof(*config) ||
      config->abi_version != ORM_C_ABI_VERSION || config->row_shape == NULL ||
      out_publisher == NULL || cflow_publisher_valid(out_publisher)) {
    orm_error_set(error, out_publisher != NULL && cflow_publisher_valid(out_publisher)
                             ? ORM_STATUS_INVALID_STATE
                             : ORM_STATUS_INVALID_ARGUMENT,
                  "invalid ORM row Publisher open");
    return out_publisher != NULL && cflow_publisher_valid(out_publisher)
               ? ORM_STATUS_INVALID_STATE
               : ORM_STATUS_INVALID_ARGUMENT;
  }
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  status = orm_owner_admit(&query->owner);
  if (status != ORM_STATUS_OK) {
    orm_error_set(error, status, "query cannot admit a row Publisher");
    return status;
  }
  connection = query->connection;
  status = orm_connection_business_status(connection, error);
  if (status != ORM_STATUS_OK) goto release_query;
  if (transaction != NULL) {
    status = orm_transaction_admit(transaction, error);
    if (status != ORM_STATUS_OK) goto release_query;
    transaction_held = true;
  }
#endif
  if (query->plan.kind != ORM_QUERY_SELECT &&
      !(query->plan.kind == ORM_QUERY_RAW && orm_query_returns_rows(&query->plan))) {
    status = ORM_STATUS_INVALID_ARGUMENT;
    orm_error_set(error, status, "invalid ORM row Publisher open");
    goto release_query;
  }
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  /* Cover native open, shape configuration and any failed-open teardown as one
   * interval. The cached parent must survive teardown consuming the query. */
  status = orm_connection_begin_native(connection, error);
  if (status != ORM_STATUS_OK) goto release_query;
  native_held = true;
#endif
  publisher_config = (orm_row_publisher_config)ORM_ROW_PUBLISHER_CONFIG_INIT(
      config->row_shape, config->scratch_bytes, config->max_depth,
      config->max_container_items, config->max_buffer_bytes);
  status = orm_row_publisher_prepare(&publisher_config, &prepared, error);
  if (status != ORM_STATUS_OK) goto release_query;
  status = database != NULL
               ? database->ops->open_cursor(
                     database->context, &query->plan,
                     &query->connection->limits, &cursor, error)
               : transaction->backend.ops->open_cursor(
                     transaction->backend.context, &query->plan,
                     &query->connection->limits, &cursor, error);
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_connection_record_native_error(connection, status);
#endif
  if (status != ORM_STATUS_OK)
    goto release_query;
  if (!orm_row_cursor_valid(&cursor)) {
    orm_row_cursor_dispose(&cursor);
    orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                  "ORM backend returned an invalid cursor");
    status = ORM_STATUS_INTERNAL_ERROR;
    goto release_query;
  }
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  cursor.owner = query;
  cursor.release_owner = orm_query_release_execution;
  cursor.owner_status = orm_query_execution_status;
  cursor.report_owner_error = orm_query_report_native_error;
  cursor.begin_execution = orm_query_begin_row_execution;
  cursor.end_execution = orm_query_end_row_execution;
  cursor.request_cleanup = orm_query_request_row_cleanup;
  cursor.transaction_owner = transaction;
  cursor.release_transaction_owner = transaction != NULL
      ? orm_transaction_release_execution : NULL;
#endif
  wait_timeout_ns = cursor.wait_timeout_ns;
  status = orm_row_publisher_publish(out_publisher, &cursor, prepared, error);
  if (status == ORM_STATUS_OK) prepared = NULL;
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  /* configure_shape is a native callback too; publish its terminal failure
   * before disposing the cursor or releasing any of its parent holds. */
  orm_connection_record_native_error(connection, status);
#endif
  if (status != ORM_STATUS_OK) {
    orm_row_publisher_prepared_destroy(prepared);
    orm_row_cursor_dispose(&cursor);
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
    orm_connection_end_native(connection, status);
#endif
    return status;
  }
  if (wait_timeout_ns != 0u &&
      !cflow_publisher_timeout(&timed_publisher, out_publisher,
                            cflow_duration_from_ns(wait_timeout_ns))) {
    cflow_publisher_destroy(out_publisher);
    memset(out_publisher, 0, sizeof(*out_publisher));
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate ORM row WAIT timeout source");
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
    orm_connection_end_native(connection, ORM_STATUS_OUT_OF_MEMORY);
#endif
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  if (cflow_publisher_valid(&timed_publisher)) *out_publisher = timed_publisher;
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_connection_end_native(connection, status);
#endif
  return status;
release_query:
  orm_row_publisher_prepared_destroy(prepared);
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  if (transaction_held) orm_transaction_release_execution(transaction);
  orm_query_release_execution(query);
  if (native_held) orm_connection_end_native(connection, status);
#endif
  return status;
}

uint32_t ORM_C_CALL orm_c_abi_version(void) { return ORM_C_ABI_VERSION; }

const char *ORM_C_CALL orm_status_message(orm_status_t status) {
  switch (status) {
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
    case ORM_OWNER_STATUS_COMMIT_UNKNOWN: return "commit outcome unknown";
    case ORM_OWNER_STATUS_CLEANUP_FAILED: return "cleanup failed";
#endif
    case ORM_STATUS_OK: return "ok";
    case ORM_STATUS_INVALID_ARGUMENT: return "invalid argument";
    case ORM_STATUS_ABI_MISMATCH: return "ABI mismatch";
    case ORM_STATUS_OUT_OF_MEMORY: return "out of memory";
    case ORM_STATUS_CONNECTION_ERROR: return "connection error";
    case ORM_STATUS_SQL_ERROR: return "SQL error";
    case ORM_STATUS_TYPE_ERROR: return "type error";
    case ORM_STATUS_OUT_OF_RANGE: return "out of range";
    case ORM_STATUS_LIMIT_EXCEEDED: return "limit exceeded";
    case ORM_STATUS_INVALID_STATE: return "invalid state";
    case ORM_STATUS_NULL_VALUE: return "null value";
    case ORM_STATUS_INTERNAL_ERROR: return "internal error";
    case ORM_STATUS_BUSY: return "busy";
    case ORM_STATUS_UNSUPPORTED: return "unsupported";
    case ORM_STATUS_DATASTORE_ERROR: return "datastore error";
    case ORM_STATUS_CONSTRAINT: return "constraint violation";
    default: return "unknown ORM status";
  }
}

void ORM_C_CALL orm_error_init(orm_error_t *error) {
  if (error == NULL)
    return;
  memset(error, 0, sizeof(*error));
  error->struct_size = sizeof(*error);
}

void ORM_C_CALL orm_config(orm_config_t *config) {
  if (config == NULL)
    return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = ORM_C_ABI_VERSION;
  config->max_parameters = ORM_C_DEFAULT_MAX_PARAMETERS;
  config->max_columns = ORM_C_DEFAULT_MAX_COLUMNS;
  config->max_predicates = ORM_C_DEFAULT_MAX_PREDICATES;
  config->max_assignments = ORM_C_DEFAULT_MAX_ASSIGNMENTS;
  config->max_query_bytes = ORM_C_DEFAULT_MAX_QUERY_BYTES;
  config->max_parameter_bytes = ORM_C_DEFAULT_MAX_PARAMETER_BYTES;
  config->max_result_rows = ORM_C_DEFAULT_MAX_RESULT_ROWS;
  config->max_result_bytes = ORM_C_DEFAULT_MAX_RESULT_BYTES;
}

void ORM_C_CALL orm_flow_config(orm_flow_config_t *config,
                               const cmeta_data_desc *row_shape) {
  if (config == NULL)
    return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = ORM_C_ABI_VERSION;
  config->row_shape = row_shape;
  config->scratch_bytes = ORM_C_DEFAULT_FLOW_SCRATCH_BYTES;
  config->max_depth = ORM_C_DEFAULT_FLOW_MAX_DEPTH;
  config->max_container_items = ORM_C_DEFAULT_FLOW_MAX_CONTAINER_ITEMS;
  config->max_buffer_bytes = ORM_C_DEFAULT_FLOW_MAX_BUFFER_BYTES;
}

orm_status_t ORM_C_CALL orm_connect(const orm_config_t *config,
                                   orm_connection_t **out_connection,
                                   orm_error_t *error) {
  return orm_connect_with_factory_v1(config, orm_backend_create,
                                     out_connection, error);
}

orm_status_t ORM_C_CALL orm_connect_with_factory_v1(
    const orm_config_t *config, orm_backend_factory_v1 factory,
    orm_connection_t **out_connection, orm_error_t *error) {
  orm_connection_t *connection;
  orm_status_t status;
  if (out_connection != NULL)
    *out_connection = NULL;
  if (out_connection == NULL || factory == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid ORM connection factory arguments");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  connection = (orm_connection_t *)calloc(1u, sizeof(*connection));
  if (connection == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate ORM connection");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  status = orm_limits_from_config(config, &connection->limits, error);
  if (status == ORM_STATUS_OK)
    status = factory(config, &connection->limits, &connection->backend, error);
  if (status != ORM_STATUS_OK) {
    free(connection);
    return status;
  }
  if (!orm_backend_valid(&connection->backend)) {
    if (connection->backend.ops != NULL &&
        connection->backend.ops->destroy != NULL &&
        connection->backend.context != NULL)
      connection->backend.ops->destroy(connection->backend.context);
    free(connection);
    orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                  "ORM backend factory returned an invalid handle");
    return ORM_STATUS_INTERNAL_ERROR;
  }
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  status = orm_owner_init(&connection->owner, ORM_OWNER_DEFAULT_REFERENCES,
                          ORM_OWNER_DEFAULT_DEPENDENTS);
  if (status != ORM_STATUS_OK) {
    connection->backend.ops->destroy(connection->backend.context);
    free(connection);
    orm_error_set(error, status, "initialize connection owner");
    return status;
  }
#endif
  *out_connection = connection;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

void ORM_C_CALL orm_disconnect(orm_connection_t *connection) {
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_connection_release(connection);
#else
  if (connection == NULL) return;
  if (connection->backend.ops != NULL && connection->backend.context != NULL)
    connection->backend.ops->destroy(connection->backend.context);
  free(connection);
#endif
}

orm_status_t ORM_C_CALL orm_transaction_begin(
    orm_connection_t *connection, orm_isolation_t isolation,
    orm_transaction_t **out_transaction, orm_error_t *error) {
  orm_transaction_t *transaction;
  orm_status_t status;
  if (out_transaction != NULL)
    *out_transaction = NULL;
  if (connection == NULL ||
      out_transaction == NULL || !orm_valid_isolation(isolation)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid ORM transaction arguments");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  status = orm_connection_admit_business(connection);
  if (status != ORM_STATUS_OK) {
    orm_error_set(error, status, "connection cannot admit a transaction");
    return status;
  }
#endif
  if (!orm_backend_valid(&connection->backend)) {
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
    orm_connection_release_child(connection);
#endif
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "invalid ORM transaction arguments");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  transaction = (orm_transaction_t *)calloc(1u, sizeof(*transaction));
  if (transaction == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate ORM transaction");
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
    orm_connection_release_child(connection);
#endif
    return ORM_STATUS_OUT_OF_MEMORY;
  }
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  status = orm_owner_init(&transaction->owner, ORM_OWNER_DEFAULT_REFERENCES,
                          ORM_OWNER_DEFAULT_DEPENDENTS);
  if (status != ORM_STATUS_OK) {
    free(transaction);
    orm_connection_release_child(connection);
    orm_error_set(error, status, "initialize transaction owner");
    return status;
  }
  transaction->connection = connection;
  status = orm_connection_begin_native(connection, error);
  if (status != ORM_STATUS_OK) {
    orm_transaction_action(transaction,
                           orm_owner_release_reference(&transaction->owner));
    return status;
  }
#endif
  status = connection->backend.ops->begin_transaction(
      connection->backend.context, isolation, &transaction->backend, error);
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_connection_record_native_error(connection, status);
#endif
  if (status != ORM_STATUS_OK) {
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
    transaction->backend = (orm_transaction_backend){0};
    orm_transaction_action(transaction,
                           orm_owner_release_reference(&transaction->owner));
    orm_connection_end_native(connection, status);
#else
    free(transaction);
#endif
    return status;
  }
  if (!orm_transaction_backend_valid(&transaction->backend)) {
    if (transaction->backend.ops != NULL &&
        transaction->backend.ops->destroy != NULL &&
        transaction->backend.context != NULL)
      transaction->backend.ops->destroy(transaction->backend.context);
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
    transaction->backend = (orm_transaction_backend){0};
    orm_transaction_action(transaction,
                           orm_owner_release_reference(&transaction->owner));
    orm_connection_end_native(connection, ORM_STATUS_INTERNAL_ERROR);
#else
    free(transaction);
#endif
    orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                  "backend returned an invalid transaction");
    return ORM_STATUS_INTERNAL_ERROR;
  }
  transaction->connection = connection;
  transaction->state = ORM_TRANSACTION_ACTIVE;
  *out_transaction = transaction;
  orm_error_set(error, ORM_STATUS_OK, NULL);
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_connection_end_native(connection, ORM_STATUS_OK);
#endif
  return ORM_STATUS_OK;
}

static orm_status_t orm_transaction_finish(orm_transaction_t *transaction,
                                           bool commit,
                                           orm_error_t *error) {
  orm_status_t status;
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  status = orm_transaction_begin_operation(transaction, error);
  if (status != ORM_STATUS_OK) return status;
#else
  if (transaction == NULL ||
      !orm_transaction_backend_valid(&transaction->backend) ||
      transaction->state != ORM_TRANSACTION_ACTIVE) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "ORM transaction is not active");
    return ORM_STATUS_INVALID_STATE;
  }
#endif
  status = commit ? transaction->backend.ops->commit(
                        transaction->backend.context, error)
                  : transaction->backend.ops->rollback(
                        transaction->backend.context, error);
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  /* Once COMMIT was dispatched, connection loss cannot prove non-commit.
   * Keep an explicit native unknown as well; neither path is replayable. */
  if (commit && status == ORM_STATUS_CONNECTION_ERROR) {
    status = ORM_OWNER_STATUS_COMMIT_UNKNOWN;
    if (error != NULL && error->struct_size >= sizeof(*error)) {
      error->status = status; /* Preserve the native diagnostic, not overlapping copy. */
      if (error->message[0] == '\0')
        orm_error_set(error, status, "commit outcome is unknown");
    }
  }
  orm_transaction_end_operation(transaction, status,
      commit ? ORM_TRANSACTION_COMMITTED : ORM_TRANSACTION_ROLLED_BACK);
#else
  if (status == ORM_STATUS_OK)
    transaction->state = commit ? ORM_TRANSACTION_COMMITTED
                                : ORM_TRANSACTION_ROLLED_BACK;
#endif
  return status;
}

orm_status_t ORM_C_CALL orm_transaction_commit(orm_transaction_t *transaction,
                                              orm_error_t *error) {
  return orm_transaction_finish(transaction, true, error);
}

orm_status_t ORM_C_CALL orm_transaction_rollback(
    orm_transaction_t *transaction, orm_error_t *error) {
  return orm_transaction_finish(transaction, false, error);
}

typedef orm_status_t (*orm_savepoint_fn)(void *, vstr, orm_error_t *);
typedef enum orm_savepoint_kind {
  ORM_SAVEPOINT_CREATE,
  ORM_SAVEPOINT_ROLLBACK,
  ORM_SAVEPOINT_RELEASE
} orm_savepoint_kind;

static orm_status_t orm_transaction_savepoint_call(
    orm_transaction_t *transaction, vstr name, orm_savepoint_kind operation,
    orm_error_t *error) {
  orm_status_t status;
  orm_savepoint_fn function = NULL;
  if (transaction == NULL || !orm_view_valid(name, false) ||
      memchr(name.data, '\0', name.len) != NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "invalid ORM savepoint operation");
    return ORM_STATUS_INVALID_STATE;
  }
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  status = orm_transaction_begin_operation(transaction, error);
  if (status != ORM_STATUS_OK) return status;
#else
  if (transaction->state != ORM_TRANSACTION_ACTIVE ||
      !orm_transaction_backend_valid(&transaction->backend)) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "invalid ORM savepoint operation");
    return ORM_STATUS_INVALID_STATE;
  }
#endif
  /* Admission must precede reading the function table: checked-close clears it.
   * Its completion hold also prevents concurrent cleanup during native calls. */
  switch (operation) {
  case ORM_SAVEPOINT_CREATE: function = transaction->backend.ops->savepoint; break;
  case ORM_SAVEPOINT_ROLLBACK:
    function = transaction->backend.ops->rollback_to_savepoint; break;
  case ORM_SAVEPOINT_RELEASE:
    function = transaction->backend.ops->release_savepoint; break;
  }
  if (function == NULL) {
    status = ORM_STATUS_INVALID_STATE;
    orm_error_set(error, status, "invalid ORM savepoint operation");
  } else {
    status = function(transaction->backend.context, name, error);
  }
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_transaction_end_operation(transaction, status, ORM_TRANSACTION_ACTIVE);
#endif
  return status;
}

orm_status_t ORM_C_CALL orm_transaction_savepoint(
    orm_transaction_t *transaction, vstr name, orm_error_t *error) {
  return orm_transaction_savepoint_call(transaction, name, ORM_SAVEPOINT_CREATE,
                                        error);
}

orm_status_t ORM_C_CALL orm_transaction_rollback_to_savepoint(
    orm_transaction_t *transaction, vstr name, orm_error_t *error) {
  return orm_transaction_savepoint_call(transaction, name, ORM_SAVEPOINT_ROLLBACK,
                                        error);
}

orm_status_t ORM_C_CALL orm_transaction_release_savepoint(
    orm_transaction_t *transaction, vstr name, orm_error_t *error) {
  return orm_transaction_savepoint_call(transaction, name, ORM_SAVEPOINT_RELEASE,
                                        error);
}

void ORM_C_CALL orm_transaction_destroy(orm_transaction_t *transaction) {
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_transaction_release(transaction);
#else
  orm_error_t ignored;
  if (transaction == NULL)
    return;
  if (transaction->state == ORM_TRANSACTION_ACTIVE &&
      orm_transaction_backend_valid(&transaction->backend)) {
    orm_error_init(&ignored);
    (void)transaction->backend.ops->rollback(transaction->backend.context,
                                             &ignored);
  }
  if (transaction->backend.ops != NULL &&
      transaction->backend.ops->destroy != NULL &&
      transaction->backend.context != NULL)
    transaction->backend.ops->destroy(transaction->backend.context);
  free(transaction);
#endif
}

orm_status_t ORM_C_CALL orm_query_create(orm_connection_t *connection,
                                        vstr table, orm_query_t **out_query,
                                        orm_error_t *error) {
  return orm_query_make(connection, table, ORM_QUERY_SELECT, out_query, error);
}

orm_status_t ORM_C_CALL orm_insert(orm_connection_t *connection, vstr table,
                                  orm_query_t **out_query,
                                  orm_error_t *error) {
  return orm_query_make(connection, table, ORM_QUERY_INSERT, out_query, error);
}

orm_status_t ORM_C_CALL orm_update(orm_connection_t *connection, vstr table,
                                  orm_query_t **out_query,
                                  orm_error_t *error) {
  return orm_query_make(connection, table, ORM_QUERY_UPDATE, out_query, error);
}

orm_status_t ORM_C_CALL orm_delete(orm_connection_t *connection, vstr table,
                                  orm_query_t **out_query,
                                  orm_error_t *error) {
  return orm_query_make(connection, table, ORM_QUERY_DELETE, out_query, error);
}

orm_status_t ORM_C_CALL orm_raw(orm_connection_t *connection, vstr sql,
                               orm_query_t **out_query, orm_error_t *error) {
  return orm_query_make(connection, sql, ORM_QUERY_RAW, out_query, error);
}

void ORM_C_CALL orm_query_destroy(orm_query_t *query) {
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  orm_query_release(query);
#else
  if (query == NULL) return;
  orm_plan_destroy(&query->plan);
  free(query);
#endif
}

static orm_status_t orm_query_select_all_unlocked(orm_query_t *query,
                                            orm_error_t *error) {
  return orm_plan_select_all(query != NULL ? &query->plan : NULL, error);
}

static orm_status_t orm_query_add_column_unlocked(orm_query_t *query, vstr column,
                                            orm_error_t *error) {
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_plan_add_column(&query->plan, column, &query->connection->limits,
                             error);
}

static orm_status_t orm_query_set_unlocked(orm_query_t *query, vstr column,
                                     orm_value_t value, orm_error_t *error) {
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_plan_add_assignment(&query->plan, column, value,
                                 &query->connection->limits, error);
}

static orm_status_t orm_query_where_unlocked(orm_query_t *query, vstr column,
                                       orm_compare_t comparison,
                                       orm_value_t value,
                                       orm_error_t *error) {
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_plan_add_predicate(&query->plan, column, comparison, value,
                                &query->connection->limits, error);
}

static orm_status_t orm_query_where_key_unlocked(orm_query_t *query,
                                            const orm_key_part_t *parts,
                                            uint32_t part_count,
                                            orm_error_t *error) {
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_plan_add_key(&query->plan, parts, part_count,
                          &query->connection->limits, error);
}

static orm_status_t orm_query_bind_unlocked(orm_query_t *query, orm_value_t value,
                                      orm_error_t *error) {
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_plan_add_bind(&query->plan, value, &query->connection->limits,
                           error);
}

static orm_status_t orm_query_order_by_unlocked(orm_query_t *query, vstr column,
                                          orm_order_t order,
                                          orm_error_t *error) {
  if (query == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return orm_plan_set_order(&query->plan, column, order,
                            &query->connection->limits, error);
}

static orm_status_t orm_query_set_limit_unlocked(orm_query_t *query, uint64_t limit,
                                           orm_error_t *error) {
  if (query == NULL || query->plan.kind != ORM_QUERY_SELECT) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "limit requires a SELECT query");
    return ORM_STATUS_INVALID_STATE;
  }
  if (limit > query->connection->limits.max_result_rows) {
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "query limit exceeds max_result_rows");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  query->plan.limit = limit;
  query->plan.has_limit = true;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static orm_status_t orm_query_set_offset_unlocked(orm_query_t *query,
                                            uint64_t offset,
                                            orm_error_t *error) {
  if (query == NULL || query->plan.kind != ORM_QUERY_SELECT) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "offset requires a SELECT query");
    return ORM_STATUS_INVALID_STATE;
  }
  query->plan.offset = offset;
  query->plan.has_offset = true;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

/* Locked wrapper around the existing plan mutations. This contains no native
 * backend callback; the installed ABI 4 build keeps its original admission. */
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
#define ORM_MUTATION_RETURN(query_, error_, expression_) do { \
  if ((query_) == NULL) return (expression_); \
  orm_status_t admission_ = orm_owner_begin_write(&(query_)->owner); \
  if (admission_ != ORM_STATUS_OK) { \
    orm_error_set((error_), admission_, "query mutation is not admitted"); \
    return admission_; \
  } \
  orm_status_t result_ = (expression_); \
  orm_owner_end_write(&(query_)->owner); \
  return result_; \
} while (0)
#else
#define ORM_MUTATION_RETURN(query_, error_, expression_) return (expression_)
#endif

orm_status_t ORM_C_CALL orm_query_select_all(orm_query_t *query, orm_error_t *error) {
  ORM_MUTATION_RETURN(query, error, orm_query_select_all_unlocked(query, error));
}

orm_status_t ORM_C_CALL orm_query_add_column(orm_query_t *query, vstr column, orm_error_t *error) {
  ORM_MUTATION_RETURN(query, error, orm_query_add_column_unlocked(query, column, error));
}

orm_status_t ORM_C_CALL orm_query_set(orm_query_t *query, vstr column, orm_value_t value, orm_error_t *error) {
  ORM_MUTATION_RETURN(query, error, orm_query_set_unlocked(query, column, value, error));
}

orm_status_t ORM_C_CALL orm_query_where(orm_query_t *query, vstr column, orm_compare_t comparison, orm_value_t value, orm_error_t *error) {
  ORM_MUTATION_RETURN(query, error, orm_query_where_unlocked(query, column, comparison, value, error));
}

orm_status_t ORM_C_CALL orm_query_where_key(orm_query_t *query, const orm_key_part_t *parts, uint32_t part_count, orm_error_t *error) {
  ORM_MUTATION_RETURN(query, error, orm_query_where_key_unlocked(query, parts, part_count, error));
}

orm_status_t ORM_C_CALL orm_query_bind(orm_query_t *query, orm_value_t value, orm_error_t *error) {
  ORM_MUTATION_RETURN(query, error, orm_query_bind_unlocked(query, value, error));
}

orm_status_t ORM_C_CALL orm_query_order_by(orm_query_t *query, vstr column, orm_order_t order, orm_error_t *error) {
  ORM_MUTATION_RETURN(query, error, orm_query_order_by_unlocked(query, column, order, error));
}

orm_status_t ORM_C_CALL orm_query_set_limit(orm_query_t *query, uint64_t limit, orm_error_t *error) {
  ORM_MUTATION_RETURN(query, error, orm_query_set_limit_unlocked(query, limit, error));
}

orm_status_t ORM_C_CALL orm_query_set_offset(orm_query_t *query, uint64_t offset, orm_error_t *error) {
  ORM_MUTATION_RETURN(query, error, orm_query_set_offset_unlocked(query, offset, error));
}

#undef ORM_MUTATION_RETURN

orm_status_t ORM_C_CALL orm_query_open_flow(
    orm_query_t *query, const orm_flow_config_t *config,
    cflow_publisher *out_publisher, orm_error_t *error) {
  return orm_open_rows(query,
                       query != NULL ? &query->connection->backend : NULL,
                       NULL, config, out_publisher, error);
}

orm_status_t ORM_C_CALL orm_query_open_flow_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction,
    const orm_flow_config_t *config, cflow_publisher *out_publisher,
    orm_error_t *error) {
  if (query == NULL || transaction == NULL ||
#if !defined(ORM_NATIVE_OWNER_CANDIDATE)
      transaction->state != ORM_TRANSACTION_ACTIVE ||
#endif
      query->connection != transaction->connection) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "query and transaction do not share an active connection");
    return ORM_STATUS_INVALID_STATE;
  }
  return orm_open_rows(query, NULL, transaction, config, out_publisher,
                       error);
}

orm_status_t ORM_C_CALL orm_query_open_command_flow(
    orm_query_t *query, cflow_publisher *out_publisher, orm_error_t *error) {
  return orm_open_command(query,
                          query != NULL ? &query->connection->backend : NULL,
                          NULL, out_publisher, error);
}

orm_status_t ORM_C_CALL orm_query_open_command_flow_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction,
    cflow_publisher *out_publisher, orm_error_t *error) {
  if (query == NULL || transaction == NULL ||
#if !defined(ORM_NATIVE_OWNER_CANDIDATE)
      transaction->state != ORM_TRANSACTION_ACTIVE ||
#endif
      query->connection != transaction->connection) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "query and transaction do not share an active connection");
    return ORM_STATUS_INVALID_STATE;
  }
  return orm_open_command(query, NULL, transaction, out_publisher,
                          error);
}
