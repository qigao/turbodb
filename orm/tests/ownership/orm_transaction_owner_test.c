/* This translation unit shares the checked-owner runner. */
#define TINYTEST_NO_MAIN
#include "orm_internal.h"
#include <tinytest.h>
#include <cmeta/struct.h>
#include <stddef.h>
#include <string.h>

/* Observers delegate to real SQLite operations without supplying any owner
 * reference. A failed lifetime assertion stops before accessing freed state. */
static orm_connection_t *connection;
static orm_transaction_t *transaction;
static orm_query_t *query;
static cflow_publisher first;
static cflow_publisher second;
static orm_error_t error;
static orm_transaction_backend_ops observed_ops;
static void (*native_destroy)(void *);
static orm_status_t (*native_rollback)(void *, orm_error_t *);
static orm_status_t (*native_commit)(void *, orm_error_t *);
static unsigned destroy_calls;
static unsigned rollback_calls;
static unsigned commit_calls;

static int probe_commit_admission;
static orm_status_t nested_admission;
static int probe_close_on_commit;
static int probe_close_on_destroy;
static int release_on_destroy;
static orm_status_t nested_close;
static orm_transaction_t *retained_transaction;

Struct(tx_owner_row, (int, id));
static const cmeta_type_identity row_identity = CMETA_TYPE_ID_ATOM_INIT("orm.owner.TxRow");
static const cmeta_type_traits row_traits = {
  .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc row_type = {
  .name = "tx_owner_row", .size = sizeof(tx_owner_row),
  .align = _Alignof(tx_owner_row), .kind = CMETA_T_OBJECT,
  .traits = &row_traits, .identity = &row_identity};
static const cmeta_data_field_desc row_fields[] = {
  {"orm.owner.TxRow.id", "id", offsetof(tx_owner_row, id), &cmeta_data_int}};
static const cmeta_data_struct_shape row_shape = {
  .layout = StructMeta(tx_owner_row), .fields = row_fields, .field_count = 1u};
static const cmeta_data_desc row_data = {
  .struct_size = offsetof(cmeta_data_desc, shape) + sizeof(((cmeta_data_desc *)0)->shape),
  .abi_version = CMETA_DATA_DESC_ABI_VERSION,
  .stable_id = "orm.owner.TxRow.data", .display_name = "TxRow",
  .kind = CMETA_DATA_STRUCT, .storage_type = &row_type, .shape = &row_shape};

static void observed_destroy(void *context) {
  ++destroy_calls;
  if (probe_close_on_destroy) {
    orm_error_t close_error;
    orm_error_init(&close_error);
    nested_close = orm_transaction_close(transaction, &close_error);
  }
  if (release_on_destroy) {
    orm_transaction_t *released = transaction;
    transaction = NULL;
    orm_transaction_release(released);
  }
  native_destroy(context);
}
static orm_status_t observed_rollback(void *context, orm_error_t *out_error) {
  ++rollback_calls;
  return native_rollback(context, out_error);
}
static orm_status_t observed_commit(void *context, orm_error_t *out_error) {
  ++commit_calls;
  if (probe_close_on_commit) {
    orm_error_t close_error;
    orm_error_init(&close_error);
    nested_close = orm_transaction_close(transaction, &close_error);
  }
  if (probe_commit_admission) {
    orm_error_t nested_error;
    orm_error_init(&nested_error);
    nested_admission = orm_query_open_command_flow_in_transaction(
        query, transaction, &second, &nested_error);
  }
  return native_commit(context, out_error);
}
static void drop_publisher(cflow_publisher *publisher) {
  if (!cflow_publisher_valid(publisher)) return;
  cflow_publisher released = *publisher;
  memset(publisher, 0, sizeof(*publisher));
  cflow_publisher_destroy(&released);
}
static void drop_transaction(void) {
  orm_transaction_t *released = transaction;
  transaction = NULL;
  orm_transaction_destroy(released);
}
static void open_command(cflow_publisher *publisher) {
  check_equal(orm_query_open_command_flow_in_transaction(
                  query, transaction, publisher, &error), ORM_STATUS_OK);
  check_true(cflow_publisher_valid(publisher));
}

static void open_rows(const char *sql) {
  orm_flow_config_t config;
  orm_query_destroy(query); query = NULL;
  check_equal(orm_raw(connection, orm_view(sql), &query, &error), ORM_STATUS_OK);
  orm_flow_config(&config, &row_data);
  check_equal(orm_query_open_flow_in_transaction(
                  query, transaction, &config, &first, &error), ORM_STATUS_OK);
}

spec("real SQLite transaction Publisher ownership") {
  (void)ttest_config__;
  before_each() {
    orm_config_t config;
    orm_option_t filename;
    orm_result_t *result = NULL;
    connection = NULL; transaction = NULL; query = NULL;
    memset(&first, 0, sizeof(first)); memset(&second, 0, sizeof(second));
    destroy_calls = 0u; rollback_calls = 0u; commit_calls = 0u;
    probe_commit_admission = 0; nested_admission = ORM_STATUS_OK;
    probe_close_on_commit = 0; probe_close_on_destroy = 0;
    release_on_destroy = 0; nested_close = ORM_STATUS_OK;
    retained_transaction = NULL;
    orm_error_init(&error); orm_config(&config);
    filename.keyword = orm_view("filename"); filename.value = orm_view(":memory:");
    config.driver = orm_view("sqlite"); config.options = &filename;
    config.option_count = 1u;
    check_equal(orm_connect(&config, &connection, &error), ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("create table tx_owned(id integer)"),
                        &query, &error), ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    orm_result_destroy(result); orm_query_destroy(query); query = NULL;
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &transaction, &error), ORM_STATUS_OK);
    observed_ops = *transaction->backend.ops;
    native_destroy = observed_ops.destroy; native_rollback = observed_ops.rollback;
    native_commit = observed_ops.commit;
    observed_ops.destroy = observed_destroy; observed_ops.rollback = observed_rollback;
    observed_ops.commit = observed_commit;
    transaction->backend.ops = &observed_ops;
    check_equal(orm_raw(connection, orm_view("insert into tx_owned values (7)"),
                        &query, &error), ORM_STATUS_OK);
  }
  after_each() {
    /* Old-core lazy command cleanup only frees command state; SQLite row cursor
     * cleanup only finalizes its statement. Neither reads the freed transaction.
     * Never resume after a failed retention check. */
    drop_publisher(&second); drop_publisher(&first);
    /* On a failing case, the extra reference still belongs to this fixture. */
    if (retained_transaction != NULL) {
      orm_transaction_t *released = retained_transaction;
      retained_transaction = NULL;
      orm_transaction_release(released);
    }
    orm_query_destroy(query); query = NULL;
    drop_transaction(); orm_disconnect(connection); connection = NULL;
  }
  it("normally releases the transaction once after its Publisher") {
    open_command(&first); drop_publisher(&first);
    check_equal(destroy_calls, 0u);
    drop_transaction();
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
    check_equal(commit_calls, 0u);
  }
  it("retains a released transaction until its unconsumed Publisher is destroyed") {
    open_command(&first); drop_transaction();
    check_equal(destroy_calls, 0u); check_equal(rollback_calls, 0u);
    drop_publisher(&first);
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
    check_equal(commit_calls, 0u);
  }
  it("can execute the admitted command after the external transaction is released") {
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    open_command(&first); drop_transaction();
    check_equal(destroy_calls, 0u);
    const cflow_step step = cflow_publisher_resume(&first, NULL, &result);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(result.affected_rows, (uint64_t)1u);
    check_equal(rollback_calls, 0u); check_equal(destroy_calls, 0u);
    drop_publisher(&first);
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
    check_equal(commit_calls, 0u);
  }
  it("does not drop the transaction lease on cancellation") {
    open_command(&first); cflow_publisher_cancel(&first); drop_transaction();
    check_equal(destroy_calls, 0u); check_equal(rollback_calls, 0u);
    drop_publisher(&first);
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
  }
  it("retains a transaction until both Publishers have been destroyed") {
    open_command(&first); open_command(&second); drop_transaction();
    check_equal(destroy_calls, 0u);
    drop_publisher(&first); check_equal(destroy_calls, 0u);
    drop_publisher(&second);
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
  }
  it("rejects commit before native execution while an unconsumed Publisher exists") {
    open_command(&first);
    const orm_status_t status = orm_transaction_commit(transaction, &error);
    check_equal(status, ORM_STATUS_BUSY); check_equal(commit_calls, 0u);
    check_equal(transaction->state, ORM_TRANSACTION_ACTIVE);
    drop_publisher(&first);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(commit_calls, 1u);
    drop_transaction(); check_equal(rollback_calls, 0u); check_equal(destroy_calls, 1u);
  }
  it("rejects rollback before native execution while an unconsumed Publisher exists") {
    open_command(&first);
    const orm_status_t status = orm_transaction_rollback(transaction, &error);
    check_equal(status, ORM_STATUS_BUSY); check_equal(rollback_calls, 0u);
    check_equal(transaction->state, ORM_TRANSACTION_ACTIVE);
    drop_publisher(&first);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    check_equal(rollback_calls, 1u);
    drop_transaction(); check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
  }
  it("keeps commit blocked after cancel until the Publisher handle is consumed") {
    open_command(&first); cflow_publisher_cancel(&first);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_BUSY);
    check_equal(commit_calls, 0u);
    drop_publisher(&first);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
  }
  it("does not retain a transaction after rejected Publisher creation") {
    check_equal(orm_query_open_command_flow_in_transaction(
                    query, transaction, NULL, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    drop_transaction();
    check_equal(destroy_calls, 1u); check_equal(rollback_calls, 0u);
  }
  it("retains a released transaction through actual row decoding") {
    tx_owner_row row = {0};
    open_rows("select 7 as id"); drop_transaction();
    check_equal(destroy_calls, 0u);
    const cflow_step step = cflow_publisher_resume(&first, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE); check_equal(row.id, 7);
    check_equal(rollback_calls, 0u); check_equal(destroy_calls, 0u);
    drop_publisher(&first);
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
  }
  it("rejects commit while an unconsumed row cursor exists") {
    open_rows("select 7 as id");
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_BUSY);
    check_equal(commit_calls, 0u);
    drop_publisher(&first);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
  }
  it("retains the cancelled row cursor lease until its Publisher is destroyed") {
    open_rows("select 7 as id"); cflow_publisher_cancel(&first);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_BUSY);
    check_equal(rollback_calls, 0u);
    drop_publisher(&first);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
  }
  it("releases transaction admission after native row open fails") {
    orm_flow_config_t config;
    orm_query_destroy(query); query = NULL;
    check_equal(orm_raw(connection, orm_view("select id from missing_tx_table"),
                        &query, &error), ORM_STATUS_OK);
    orm_flow_config(&config, &row_data);
    check_equal(orm_query_open_flow_in_transaction(query, transaction, &config,
                                                   &first, &error), ORM_STATUS_SQL_ERROR);
    check_false(cflow_publisher_valid(&first));
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    drop_transaction(); check_equal(destroy_calls, 1u); check_equal(rollback_calls, 0u);
  }
  it("rejects new Publisher admission during an actual native commit callback") {
    probe_commit_admission = 1;
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(nested_admission, ORM_STATUS_BUSY);
    check_false(cflow_publisher_valid(&second)); check_equal(commit_calls, 1u);
  }
  it("rejects savepoint creation while a command Publisher exists") {
    open_command(&first);
    check_equal(orm_transaction_savepoint(transaction, orm_view("held"), &error),
                ORM_STATUS_BUSY);
    drop_publisher(&first);
    check_equal(orm_transaction_savepoint(transaction, orm_view("held"), &error),
                ORM_STATUS_OK);
  }
  it("rejects rollback to a savepoint while a command Publisher exists") {
    check_equal(orm_transaction_savepoint(transaction, orm_view("held"), &error),
                ORM_STATUS_OK);
    open_command(&first);
    check_equal(orm_transaction_rollback_to_savepoint(
                    transaction, orm_view("held"), &error), ORM_STATUS_BUSY);
    drop_publisher(&first);
    check_equal(orm_transaction_rollback_to_savepoint(
                    transaction, orm_view("held"), &error), ORM_STATUS_OK);
  }
  it("rejects savepoint release while a command Publisher exists") {
    check_equal(orm_transaction_savepoint(transaction, orm_view("held"), &error),
                ORM_STATUS_OK);
    open_command(&first);
    check_equal(orm_transaction_release_savepoint(
                    transaction, orm_view("held"), &error), ORM_STATUS_BUSY);
    drop_publisher(&first);
    check_equal(orm_transaction_release_savepoint(
                    transaction, orm_view("held"), &error), ORM_STATUS_OK);
  }
  it("rejects a null checked-close and accepts a null release") {
    check_equal(orm_transaction_close(NULL, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(error.status, ORM_STATUS_INVALID_ARGUMENT);
    orm_transaction_release(NULL);
  }
  it("rejects close of an active transaction without rolling back or changing ownership") {
    const uint32_t references = transaction->owner.references;
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_INVALID_STATE);
    check_equal(error.status, ORM_STATUS_INVALID_STATE);
    check_equal(transaction->owner.references, references);
    check_equal(transaction->owner.dependents, 0u);
    check_equal(transaction->owner.phase, ORM_OWNER_OPEN);
    check_equal(transaction->state, ORM_TRANSACTION_ACTIVE);
    check_equal(rollback_calls, 0u); check_equal(destroy_calls, 0u);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
  }
  it("keeps checked-close BUSY until a cancelled command Publisher is destroyed") {
    open_command(&first);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_BUSY);
    check_equal(error.status, ORM_STATUS_BUSY);
    check_equal(transaction->owner.phase, ORM_OWNER_OPEN);
    check_equal(transaction->owner.dependents, 1u);
    cflow_publisher_cancel(&first);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_BUSY);
    check_equal(transaction->state, ORM_TRANSACTION_ACTIVE);
    check_equal(rollback_calls, 0u); check_equal(destroy_calls, 0u);
    drop_publisher(&first);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_INVALID_STATE);
  }
  it("keeps checked-close BUSY for an actual row cursor until teardown") {
    open_rows("select 7 as id");
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_BUSY);
    cflow_publisher_cancel(&first);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_BUSY);
    check_equal(rollback_calls, 0u); check_equal(destroy_calls, 0u);
    drop_publisher(&first);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
  }
  it("does not confuse terminal command delivery with releasing its Publisher") {
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    open_command(&first);
    const cflow_step step = cflow_publisher_resume(&first, NULL, &result);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_BUSY);
    check_equal(destroy_calls, 0u);
    drop_publisher(&first);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_INVALID_STATE);
  }
  it("closes committed resources once while retaining the connection until final release") {
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_equal(error.status, ORM_STATUS_OK);
    check_equal(transaction->owner.phase, ORM_OWNER_CLOSED);
    check_null(transaction->backend.context); check_null(transaction->backend.ops);
    check_equal(destroy_calls, 1u); check_equal(rollback_calls, 0u);
    check_equal(orm_transaction_close(transaction, NULL), ORM_STATUS_OK);
    orm_query_destroy(query); query = NULL;
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    drop_transaction();
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    check_equal(destroy_calls, 1u);
  }
  it("closes rolled-back resources idempotently without issuing another rollback") {
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
    drop_transaction(); check_equal(destroy_calls, 1u);
  }
  it("retains an explicit reference after the legacy destroy consumes its caller") {
    orm_transaction_retain(transaction); retained_transaction = transaction;
    check_equal(transaction->owner.references, 2u);
    drop_transaction();
    check_equal(destroy_calls, 0u); check_equal(rollback_calls, 0u);
    transaction = retained_transaction; retained_transaction = NULL;
    check_equal(transaction->owner.references, 1u);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
  }
  it("retains and releases a still-held closed handle without repeating native cleanup") {
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    orm_transaction_retain(transaction); retained_transaction = transaction;
    check_equal(transaction->owner.references, 2u);
    orm_transaction_t *released = transaction; transaction = NULL;
    orm_transaction_release(released);
    check_equal(destroy_calls, 1u);
    check_equal(orm_transaction_close(retained_transaction, &error), ORM_STATUS_OK);
  }
  it("rejects command Publisher admission after checked-close") {
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow_in_transaction(query, transaction, &first,
                                                           &error), ORM_STATUS_INVALID_STATE);
    check_false(cflow_publisher_valid(&first)); check_equal(destroy_calls, 1u);
  }
  it("rejects row Publisher admission after checked-close") {
    orm_flow_config_t config;
    orm_query_destroy(query); query = NULL;
    check_equal(orm_raw(connection, orm_view("select 7 as id"), &query, &error), ORM_STATUS_OK);
    orm_flow_config(&config, &row_data);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_query_open_flow_in_transaction(query, transaction, &config, &first,
                                                  &error), ORM_STATUS_INVALID_STATE);
    check_false(cflow_publisher_valid(&first));
  }
  it("rejects transaction completion after checked-close") {
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_INVALID_STATE);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_INVALID_STATE);
    check_equal(commit_calls, 1u); check_equal(rollback_calls, 0u);
  }
  it("rejects savepoint creation after checked-close without reading cleared backend ops") {
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(transaction, orm_view("closed_point"), &error),
                ORM_STATUS_INVALID_STATE);
  }
  it("rejects savepoint rollback after checked-close without reading cleared backend ops") {
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_rollback_to_savepoint(transaction, orm_view("closed_point"),
                                                      &error), ORM_STATUS_INVALID_STATE);
  }
  it("rejects savepoint release after checked-close without reading cleared backend ops") {
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_release_savepoint(transaction, orm_view("closed_point"),
                                                 &error), ORM_STATUS_INVALID_STATE);
  }
  it("reports BUSY from checked-close reentered during the actual native commit callback") {
    probe_close_on_commit = 1;
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(nested_close, ORM_STATUS_BUSY);
    check_equal(transaction->owner.dependents, 0u);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
  }
  it("reports BUSY from checked-close reentered during native resource destruction") {
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    probe_close_on_destroy = 1;
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_equal(nested_close, ORM_STATUS_BUSY);
    check_equal(destroy_calls, 1u);
  }
  it("defers final handle free until native destruction returns to checked-close") {
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    release_on_destroy = 1;
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_null(transaction); check_equal(destroy_calls, 1u);
    orm_query_destroy(query); query = NULL;
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
  }

}
