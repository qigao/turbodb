#include "budget.h"
#include <tinytest.h>

enum { STATEMENT_LIMIT = 100, TRANSACTION_LIMIT = 150 };
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static turbodb_error_t error;

static turbodb_status_t reserve_one(orm_sql_budget_resource resource, uint64_t amount) {
  orm_sql_budget_amount charge = {0};
  charge.value[resource] = amount;
  return orm_tidesdb_sql_budget_reserve(&budget, &charge, &error);
}

static void check_counters(const orm_tidesdb_sql_budget *expected) {
  for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) {
    check_equal(budget.used.value[i], expected->used.value[i]);
    check_equal(budget.peak.value[i], expected->peak.value[i]);
  }
  check_equal(budget.transaction_used.read_rows, expected->transaction_used.read_rows);
  check_equal(budget.transaction_used.read_bytes, expected->transaction_used.read_bytes);
  check_equal(budget.transaction_used.write_bytes, expected->transaction_used.write_bytes);
  check_equal(budget.statement_active, expected->statement_active);
  check_equal(budget.retained_work_bytes, expected->retained_work_bytes);
}

spec("TidesDB relational SQL budget contract") {
  before_each() {
    limits = (orm_sql_budget_limits){0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i)
      limits.statement.value[i] = STATEMENT_LIMIT;
    limits.transaction = (orm_sql_transaction_budget_amount){
      TRANSACTION_LIMIT, TRANSACTION_LIMIT, TRANSACTION_LIMIT};
    tdsql_error_init(&error);
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
  }

  it("rejects zero and contradictory limits without replacing the ledger") {
    check_equal(reserve_one(ORM_SQL_BUDGET_READ_ROWS, 1), TURBODB_STATUS_OK);
    const orm_tidesdb_sql_budget before = budget;
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    const orm_tidesdb_sql_budget idle = budget;
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) {
      orm_sql_budget_limits invalid = limits;
      invalid.statement.value[i] = 0;
      check_equal(orm_tidesdb_sql_budget_init(&budget, &invalid, &error), TURBODB_STATUS_INVALID_ARGUMENT);
      check_counters(&idle);
      check_equal(budget.limits.statement.value[i], before.limits.statement.value[i]);
    }
    orm_sql_budget_limits invalid = limits;
    invalid.transaction.read_rows = STATEMENT_LIMIT - 1;
    check_equal(orm_tidesdb_sql_budget_init(&budget, &invalid, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    invalid = limits; invalid.transaction.read_bytes = STATEMENT_LIMIT - 1;
    check_equal(orm_tidesdb_sql_budget_init(&budget, &invalid, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    invalid = limits; invalid.transaction.write_bytes = 0;
    check_equal(orm_tidesdb_sql_budget_init(&budget, &invalid, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_counters(&idle);
  }

  it("accepts exact limits for every resource and refuses the next unit atomically") {
    orm_sql_budget_amount full = limits.statement;
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, &full, &error), TURBODB_STATUS_OK);
    const orm_tidesdb_sql_budget before = budget;
    for (int i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) {
      check_equal(reserve_one((orm_sql_budget_resource)i, 1), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_counters(&before);
    }
    const orm_sql_budget_amount empty = {0};
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, &empty, &error), TURBODB_STATUS_OK);
    check_counters(&before);
  }

  it("does not partially reserve earlier dimensions when a later one exceeds its limit") {
    check_equal(reserve_one(ORM_SQL_BUDGET_READ_BYTES, STATEMENT_LIMIT), TURBODB_STATUS_OK);
    const orm_tidesdb_sql_budget before = budget;
    orm_sql_budget_amount charge = {0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) charge.value[i] = 1;
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, &charge, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_counters(&before);
    check_contains(error.message, "statement read bytes");
  }

  it("retains cumulative reads and writes across successful or failed statement cleanup") {
    orm_sql_budget_amount charge = {0};
    charge.value[ORM_SQL_BUDGET_READ_ROWS] = STATEMENT_LIMIT;
    charge.value[ORM_SQL_BUDGET_READ_BYTES] = STATEMENT_LIMIT;
    charge.value[ORM_SQL_BUDGET_WRITE_BYTES] = STATEMENT_LIMIT;
    charge.value[ORM_SQL_BUDGET_WORK_BYTES] = 1;
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, &charge, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, 1, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES], 0u);
    check_equal(budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(budget.transaction_used.read_rows, STATEMENT_LIMIT);
    check_equal(budget.transaction_used.read_bytes, STATEMENT_LIMIT);
    check_equal(budget.transaction_used.write_bytes, STATEMENT_LIMIT);
    const orm_tidesdb_sql_budget before = budget;
    const orm_sql_budget_resource cumulative[] = {
      ORM_SQL_BUDGET_READ_ROWS, ORM_SQL_BUDGET_READ_BYTES, ORM_SQL_BUDGET_WRITE_BYTES};
    for (size_t i = 0; i < sizeof(cumulative) / sizeof(cumulative[0]); ++i) {
      orm_sql_budget_amount over = {0};
      over.value[ORM_SQL_BUDGET_WORK_BYTES] = 1;
      over.value[cumulative[i]] = TRANSACTION_LIMIT - STATEMENT_LIMIT + 1;
      check_equal(orm_tidesdb_sql_budget_reserve(&budget, &over, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_counters(&before);
      check_contains(error.message, "transaction");
      check_equal(orm_tidesdb_sql_budget_release(&budget, cumulative[i], 1, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    }
    charge = (orm_sql_budget_amount){0};
    for (size_t i = 0; i < sizeof(cumulative) / sizeof(cumulative[0]); ++i)
      charge.value[cumulative[i]] = TRANSACTION_LIMIT - STATEMENT_LIMIT;
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, &charge, &error), TURBODB_STATUS_OK);
    check_equal(budget.transaction_used.read_rows, TRANSACTION_LIMIT);
    check_equal(budget.transaction_used.read_bytes, TRANSACTION_LIMIT);
    check_equal(budget.transaction_used.write_bytes, TRANSACTION_LIMIT);
  }

  it("rejects reset and duplicate begin while a statement owns the ledger") {
    check_equal(reserve_one(ORM_SQL_BUDGET_EXECUTION_STEPS, 1), TURBODB_STATUS_OK);
    const orm_tidesdb_sql_budget before = budget;
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_budget_reset_transaction(&budget, &error), TURBODB_STATUS_BUSY);
    check_counters(&before);
  }

  it("requires all live resources to be released before end") {
    const orm_sql_budget_resource live[] = {
      ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_MATERIALIZED_ROWS, ORM_SQL_BUDGET_GROUPS};
    for (size_t i = 0; i < sizeof(live) / sizeof(live[0]); ++i) {
      check_equal(reserve_one(live[i], 1), TURBODB_STATUS_OK);
      const orm_tidesdb_sql_budget before = budget;
      check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_BUSY);
      check_counters(&before);
      check_equal(orm_tidesdb_sql_budget_release(&budget, live[i], 1, &error), TURBODB_STATUS_OK);
    }
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_INVALID_STATE);
    check_equal(reserve_one(ORM_SQL_BUDGET_WORK_BYTES, 1), TURBODB_STATUS_INVALID_STATE);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, 0, &error), TURBODB_STATUS_INVALID_STATE);
  }

  it("resets transaction accounting only after statement cleanup") {
    check_equal(reserve_one(ORM_SQL_BUDGET_READ_ROWS, STATEMENT_LIMIT), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_READ_BYTES, STATEMENT_LIMIT), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_WRITE_BYTES, STATEMENT_LIMIT), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_reset_transaction(&budget, &error), TURBODB_STATUS_OK);
    check_equal(budget.transaction_used.write_bytes, 0u);
    check_equal(budget.transaction_used.read_rows, 0u);
    check_equal(budget.transaction_used.read_bytes, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES], STATEMENT_LIMIT);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_WRITE_BYTES, STATEMENT_LIMIT), TURBODB_STATUS_OK);
  }

  it("shares work capacity across retained rows sort scratch and the output slot") {
    enum { RETAINED_BYTES = 60, SCRATCH_BYTES = 32, OUTPUT_BYTES = 8 };
    orm_sql_budget_amount rows = {0};
    rows.value[ORM_SQL_BUDGET_WORK_BYTES] = RETAINED_BYTES;
    rows.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = 3;
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, &rows, &error), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_WORK_BYTES, SCRATCH_BYTES), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_WORK_BYTES, OUTPUT_BYTES), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_WORK_BYTES, 1), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, SCRATCH_BYTES, &error), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_WORK_BYTES, SCRATCH_BYTES), TURBODB_STATUS_OK);
    check_equal(budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES], STATEMENT_LIMIT);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 3u);
  }

  it("charges the old and complete new capacities during growth") {
    size_t old_bytes = 0, new_bytes = 0;
    check_equal(orm_tidesdb_sql_budget_reserve_capacity(&budget, 4, 8, 8, &old_bytes, &error), TURBODB_STATUS_OK);
    check_equal(old_bytes, 40u);
    /* 40 old + (8 * 8 + 8) new exceeds 100 even though new alone fits. */
    check_equal(orm_tidesdb_sql_budget_reserve_capacity(&budget, 8, 8, 8, &new_bytes, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(new_bytes, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], old_bytes);
    check_equal(orm_tidesdb_sql_budget_reserve_capacity(&budget, 6, 8, 8, &new_bytes, &error), TURBODB_STATUS_OK);
    check_equal(budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES], 96u);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, old_bytes, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], new_bytes);
    check_equal(budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES], 96u);
  }

  it("refunds an unused capacity reservation without erasing the original allocation or peak") {
    size_t old_bytes = 0, failed_bytes = 0;
    check_equal(orm_tidesdb_sql_budget_reserve_capacity(&budget, 4, 8, 8, &old_bytes, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_reserve_capacity(&budget, 6, 8, 8, &failed_bytes, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, failed_bytes, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], old_bytes);
    check_equal(budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES], old_bytes + failed_bytes);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, old_bytes, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  }

  it("rejects underflow and releasing consumptive or invalid dimensions") {
    check_equal(reserve_one(ORM_SQL_BUDGET_WORK_BYTES, 1), TURBODB_STATUS_OK);
    const orm_tidesdb_sql_budget before = budget;
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, 2, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_EXECUTION_STEPS, 0, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_release(&budget, (orm_sql_budget_resource)-1, 1, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_RESOURCE_COUNT, 1, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_counters(&before);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, 1, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, 1, &error), TURBODB_STATUS_INVALID_ARGUMENT);
  }

  it("checks capacity multiplication and metadata addition before changing any counters") {
    const orm_tidesdb_sql_budget before = budget;
    size_t bytes = 1;
    check_equal(orm_tidesdb_sql_budget_reserve_capacity(&budget, SIZE_MAX, 2, 0, &bytes, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_tidesdb_sql_budget_reserve_capacity(&budget, SIZE_MAX, 1, 1, &bytes, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(bytes, 1u);
    check_counters(&before);
    check_equal(orm_tidesdb_sql_budget_reserve_capacity(&budget, 0, 1, STATEMENT_LIMIT, &bytes, &error), TURBODB_STATUS_OK);
    check_equal(bytes, STATEMENT_LIMIT);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, bytes, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_reserve_capacity(&budget, 0, 1, 0, &bytes, &error), TURBODB_STATUS_OK);
    check_equal(bytes, 0u);
  }

  it("accepts maximum integer budgets without wrapping statement or transaction counters") {
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i)
      limits.statement.value[i] = UINT64_MAX;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = SIZE_MAX;
    limits.transaction = (orm_sql_transaction_budget_amount){UINT64_MAX, UINT64_MAX, UINT64_MAX};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_EXECUTION_STEPS, UINT64_MAX), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_EXECUTION_STEPS, 1), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(reserve_one(ORM_SQL_BUDGET_WRITE_BYTES, UINT64_MAX), TURBODB_STATUS_OK);
    size_t bytes = 0;
    check_equal(orm_tidesdb_sql_budget_reserve_capacity(&budget, SIZE_MAX, 1, 0, &bytes, &error), TURBODB_STATUS_OK);
    check_equal(bytes, SIZE_MAX);
    check_equal(reserve_one(ORM_SQL_BUDGET_WORK_BYTES, 1), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, bytes, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_WRITE_BYTES, 1), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES], 0u);
    check_equal(budget.transaction_used.write_bytes, UINT64_MAX);
  }

  it("rejects missing arguments without charging and allows an omitted error output") {
    const orm_tidesdb_sql_budget before = budget;
    check_equal(orm_tidesdb_sql_budget_init(NULL, &limits, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_init(&budget, NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_begin(NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_end(NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_reserve(NULL, &limits.statement, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_release(NULL, ORM_SQL_BUDGET_WORK_BYTES, 1, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_reset_transaction(NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    size_t bytes = 1;
    check_equal(orm_tidesdb_sql_budget_reserve_capacity(&budget, 1, 0, 0, &bytes, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_reserve_capacity(&budget, 1, 1, 0, NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(bytes, 1u);
    check_counters(&before);
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, &limits.statement, NULL), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, &limits.statement, NULL), TURBODB_STATUS_LIMIT_EXCEEDED);
  }
  it("carries retained owner work into the next statement without carrying consumed statement counters") {
    enum { OWNER_BYTES = 24, TEMP_BYTES = 16 };
    size_t receipt = 0;
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, 1, OWNER_BYTES, 0, &receipt, &error), TURBODB_STATUS_OK);
    check_equal(receipt, OWNER_BYTES); check_equal(budget.retained_work_bytes, OWNER_BYTES);
    check_equal(reserve_one(ORM_SQL_BUDGET_WORK_BYTES, TEMP_BYTES), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_READ_ROWS, STATEMENT_LIMIT), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_EXECUTION_STEPS, STATEMENT_LIMIT), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, TEMP_BYTES, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES], OWNER_BYTES + TEMP_BYTES);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], OWNER_BYTES);
    check_equal(budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES], OWNER_BYTES);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    check_equal(budget.transaction_used.read_rows, STATEMENT_LIMIT);
    check_equal(reserve_one(ORM_SQL_BUDGET_EXECUTION_STEPS, STATEMENT_LIMIT), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_READ_ROWS, TRANSACTION_LIMIT - STATEMENT_LIMIT + 1), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_contains(error.message, "transaction read rows");
    check_equal(orm_tidesdb_sql_budget_release_retained(&budget, receipt, &error), TURBODB_STATUS_OK);
    check_equal(budget.retained_work_bytes, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
  }
  it("keeps ordinary work rows and groups as end barriers when retained metadata is present") {
    enum { OWNER_BYTES = 24 };
    size_t receipt = 0;
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, 1, OWNER_BYTES, 0, &receipt, &error), TURBODB_STATUS_OK);
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_MATERIALIZED_ROWS, ORM_SQL_BUDGET_GROUPS};
    for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
      check_equal(reserve_one(resources[i], 1), TURBODB_STATUS_OK); const orm_tidesdb_sql_budget before = budget;
      check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_BUSY); check_counters(&before);
      check_equal(orm_tidesdb_sql_budget_release(&budget, resources[i], 1, &error), TURBODB_STATUS_OK);
    }
    const orm_tidesdb_sql_budget before = budget;
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, receipt, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_counters(&before); check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_release_retained(&budget, receipt, &error), TURBODB_STATUS_OK);
    const orm_tidesdb_sql_budget idle = budget;
    check_equal(orm_tidesdb_sql_budget_release_retained(&budget, receipt, &error), TURBODB_STATUS_INVALID_ARGUMENT); check_counters(&idle);
  }
  it("makes retained admission atomic for capacity overflow shared limits and invalid inputs") {
    enum { ORDINARY_BYTES = 80, OVER_BYTES = 21 };
    check_equal(reserve_one(ORM_SQL_BUDGET_WORK_BYTES, ORDINARY_BYTES), TURBODB_STATUS_OK);
    const orm_tidesdb_sql_budget before = budget; size_t receipt = 99;
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, 1, OVER_BYTES, 0, &receipt, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, SIZE_MAX, 2, 0, &receipt, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, SIZE_MAX, 1, 1, &receipt, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, 1, 0, 0, &receipt, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, 1, 1, 0, NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(NULL, 1, 1, 0, &receipt, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_budget_release_retained(NULL, 1, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(receipt, 99u); check_counters(&before);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, ORDINARY_BYTES, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK); const orm_tidesdb_sql_budget idle = budget;
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, 1, 1, 0, &receipt, &error), TURBODB_STATUS_INVALID_STATE);
    check_equal(receipt, 99u); check_counters(&idle);
  }
  it("prevents cumulative reset while an owner survives between statements") {
    enum { OWNER_BYTES = 24 };
    size_t receipt = 0;
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, 1, OWNER_BYTES, 0, &receipt, &error), TURBODB_STATUS_OK);
    check_equal(reserve_one(ORM_SQL_BUDGET_WRITE_BYTES, STATEMENT_LIMIT), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK); const orm_tidesdb_sql_budget idle = budget;
    check_equal(orm_tidesdb_sql_budget_reset_transaction(&budget, &error), TURBODB_STATUS_BUSY); check_counters(&idle);
    check_equal(orm_tidesdb_sql_budget_release_retained(&budget, receipt, &error), TURBODB_STATUS_OK);
    check_equal(budget.transaction_used.write_bytes, STATEMENT_LIMIT);
    check_equal(orm_tidesdb_sql_budget_reset_transaction(&budget, &error), TURBODB_STATUS_OK);
    check_equal(budget.transaction_used.write_bytes, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES], STATEMENT_LIMIT);
  }
  it("shares one work cap among multiple owners and ordinary work without double charging") {
    enum { FIRST_BYTES = 40, SECOND_BYTES = 60 };
    size_t first = 0, second = 0;
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, 1, FIRST_BYTES, 0, &first, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, 1, SECOND_BYTES, 0, &second, &error), TURBODB_STATUS_OK);
    check_equal(budget.retained_work_bytes, STATEMENT_LIMIT); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], STATEMENT_LIMIT);
    check_equal(reserve_one(ORM_SQL_BUDGET_WORK_BYTES, 1), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_release_retained(&budget, first, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], SECOND_BYTES);
    check_equal(reserve_one(ORM_SQL_BUDGET_WORK_BYTES, FIRST_BYTES), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_release_retained(&budget, second, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], FIRST_BYTES); check_equal(budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES], STATEMENT_LIMIT);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, FIRST_BYTES, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  }
  it("preserves SIZE_MAX retained work through statement boundaries without overflow") {
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = SIZE_MAX;
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    size_t receipt = 0, excess = 99;
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, SIZE_MAX, 1, 0, &receipt, &error), TURBODB_STATUS_OK);
    check_equal(receipt, SIZE_MAX); const orm_tidesdb_sql_budget before = budget;
    check_equal(orm_tidesdb_sql_budget_reserve_retained_capacity(&budget, 1, 1, 0, &excess, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(excess, 99u); check_counters(&before);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], SIZE_MAX); check_equal(budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES], SIZE_MAX);
    check_equal(orm_tidesdb_sql_budget_release_retained(&budget, receipt, &error), TURBODB_STATUS_OK);
    check_equal(budget.retained_work_bytes, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
  }

}
