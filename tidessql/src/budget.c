#include "budget.h"
#include "error.h"

#include <stdio.h>

static const char *const resource_names[ORM_SQL_BUDGET_RESOURCE_COUNT] = {
  "work bytes", "materialized rows", "groups", "AST nodes", "plan nodes",
  "join pairs", "execution steps", "write rows", "write bytes", "read rows",
  "read bytes"
};
static const char budget_required[] = "TidesDB SQL budget is required";

static turbodb_status_t budget_error(turbodb_error_t *error, turbodb_status_t status,
                                 const char *reason) {
  tdsql_error_set(error, status, reason);
  return status;
}

static turbodb_status_t limit_error(turbodb_error_t *error, const char *scope,
                                const char *resource) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message),
                "TidesDB SQL reserve exceeds %s %s budget", scope, resource);
  return budget_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, message);
}

static bool releasable(orm_sql_budget_resource resource) {
  return resource == ORM_SQL_BUDGET_WORK_BYTES ||
         resource == ORM_SQL_BUDGET_MATERIALIZED_ROWS ||
         resource == ORM_SQL_BUDGET_GROUPS;
}

static turbodb_status_t require_active(const orm_tidesdb_sql_budget *budget,
                                    turbodb_error_t *error) {
  if (!budget)
    return budget_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        budget_required);
  if (!budget->statement_active)
    return budget_error(error, TURBODB_STATUS_INVALID_STATE,
                        "TidesDB SQL budget requires an active statement");
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_budget_init(orm_tidesdb_sql_budget *out,
    const orm_sql_budget_limits *limits, turbodb_error_t *error) {
  if (!out || !limits)
    return budget_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        "TidesDB SQL budget init requires output and limits");
  for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) {
    if (!limits->statement.value[i])
      return budget_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                          "TidesDB SQL statement budgets must be positive");
  }
  if (limits->statement.value[ORM_SQL_BUDGET_WORK_BYTES] > SIZE_MAX ||
      limits->transaction.read_rows < limits->statement.value[ORM_SQL_BUDGET_READ_ROWS] ||
      limits->transaction.read_bytes < limits->statement.value[ORM_SQL_BUDGET_READ_BYTES] ||
      limits->transaction.write_bytes < limits->statement.value[ORM_SQL_BUDGET_WRITE_BYTES])
    return budget_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        "TidesDB SQL budget limits are inconsistent");
  *out = (orm_tidesdb_sql_budget){.limits = *limits};
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_budget_begin(orm_tidesdb_sql_budget *budget,
    turbodb_error_t *error) {
  if (!budget)
    return budget_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        budget_required);
  if (budget->statement_active)
    return budget_error(error, TURBODB_STATUS_BUSY,
                        "TidesDB SQL budget already has an active statement");
  budget->used = (orm_sql_budget_amount){0};
  budget->peak = (orm_sql_budget_amount){0};
  budget->used.value[ORM_SQL_BUDGET_WORK_BYTES] = budget->retained_work_bytes;
  budget->peak.value[ORM_SQL_BUDGET_WORK_BYTES] = budget->retained_work_bytes;
  budget->statement_active = true;
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_budget_end(orm_tidesdb_sql_budget *budget,
    turbodb_error_t *error) {
  turbodb_status_t status = require_active(budget, error);
  if (status != TURBODB_STATUS_OK) return status;
  for (int i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) {
    const uint64_t retained = i == ORM_SQL_BUDGET_WORK_BYTES ? budget->retained_work_bytes : 0;
    if (releasable((orm_sql_budget_resource)i) && budget->used.value[i] != retained)
      return budget_error(error, TURBODB_STATUS_BUSY,
                          "TidesDB SQL statement still owns budgeted resources");
  }
  budget->statement_active = false;
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_budget_reserve(orm_tidesdb_sql_budget *budget,
    const orm_sql_budget_amount *amount, turbodb_error_t *error) {
  turbodb_status_t status = require_active(budget, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (!amount)
    return budget_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        "TidesDB SQL reserve requires an amount");
  /* Copy before mutation: the input may be a view of this ledger's counters. */
  const orm_sql_budget_amount charge = *amount;
  for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) {
    if (charge.value[i] > budget->limits.statement.value[i] - budget->used.value[i])
      return limit_error(error, "statement", resource_names[i]);
  }
  const orm_sql_transaction_budget_amount tx = {
    charge.value[ORM_SQL_BUDGET_READ_ROWS],
    charge.value[ORM_SQL_BUDGET_READ_BYTES],
    charge.value[ORM_SQL_BUDGET_WRITE_BYTES]
  };
  if (tx.read_rows > budget->limits.transaction.read_rows - budget->transaction_used.read_rows)
    return limit_error(error, "transaction", "read rows");
  if (tx.read_bytes > budget->limits.transaction.read_bytes - budget->transaction_used.read_bytes)
    return limit_error(error, "transaction", "read bytes");
  if (tx.write_bytes > budget->limits.transaction.write_bytes - budget->transaction_used.write_bytes)
    return limit_error(error, "transaction", "write bytes");

  for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) {
    budget->used.value[i] += charge.value[i];
    if (budget->used.value[i] > budget->peak.value[i])
      budget->peak.value[i] = budget->used.value[i];
  }
  budget->transaction_used.read_rows += tx.read_rows;
  budget->transaction_used.read_bytes += tx.read_bytes;
  budget->transaction_used.write_bytes += tx.write_bytes;
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_budget_reserve_capacity(orm_tidesdb_sql_budget *budget,
    size_t capacity, size_t element_bytes, size_t overhead_bytes,
    size_t *reserved, turbodb_error_t *error) {
  turbodb_status_t status = require_active(budget, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (!reserved || !element_bytes)
    return budget_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        "TidesDB SQL capacity reserve requires output and element size");
  if (capacity > (SIZE_MAX - overhead_bytes) / element_bytes)
    return budget_error(error, TURBODB_STATUS_LIMIT_EXCEEDED,
                        "TidesDB SQL capacity byte calculation overflows size_t");
  const size_t bytes = capacity * element_bytes + overhead_bytes;
  orm_sql_budget_amount charge = {0};
  charge.value[ORM_SQL_BUDGET_WORK_BYTES] = bytes;
  status = orm_tidesdb_sql_budget_reserve(budget, &charge, error);
  if (status == TURBODB_STATUS_OK) *reserved = bytes;
  return status;
}

turbodb_status_t orm_tidesdb_sql_budget_release(orm_tidesdb_sql_budget *budget,
    orm_sql_budget_resource resource, uint64_t amount, turbodb_error_t *error) {
  turbodb_status_t status = require_active(budget, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (!releasable(resource) || amount > budget->used.value[resource] ||
      (resource == ORM_SQL_BUDGET_WORK_BYTES && budget->used.value[resource] - amount < budget->retained_work_bytes))
    return budget_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        "TidesDB SQL release requires owned live resources");
  budget->used.value[resource] -= amount;
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_budget_reserve_retained_capacity(orm_tidesdb_sql_budget *budget,
    size_t capacity, size_t element_bytes, size_t overhead_bytes,
    size_t *reserved, turbodb_error_t *error) {
  if (!reserved) return budget_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "TidesDB SQL retained reserve requires output");
  size_t bytes = 0;
  const turbodb_status_t status = orm_tidesdb_sql_budget_reserve_capacity(budget,
      capacity, element_bytes, overhead_bytes, &bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  /* reserve_capacity checked total work <= SIZE_MAX; retained is a subset. */
  budget->retained_work_bytes += bytes;
  *reserved = bytes;
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_budget_release_retained(orm_tidesdb_sql_budget *budget,
    uint64_t amount, turbodb_error_t *error) {
  if (!budget) return budget_error(error, TURBODB_STATUS_INVALID_ARGUMENT, budget_required);
  if (amount > budget->retained_work_bytes || amount > budget->used.value[ORM_SQL_BUDGET_WORK_BYTES])
    return budget_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "TidesDB SQL release requires owned retained work");
  budget->retained_work_bytes -= amount;
  budget->used.value[ORM_SQL_BUDGET_WORK_BYTES] -= amount;
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_budget_reset_transaction(
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  if (!budget)
    return budget_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        budget_required);
  if (budget->statement_active || budget->retained_work_bytes)
    return budget_error(error, TURBODB_STATUS_BUSY,
                        "TidesDB SQL transaction budget still has an active statement or retained owner");
  budget->transaction_used = (orm_sql_transaction_budget_amount){0};
  return TURBODB_STATUS_OK;
}
