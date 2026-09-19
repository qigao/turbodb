#include "orm_owner.h"
#include <tinytest.h>
#include <string.h>

static orm_owner owner;
enum { TEST_REFERENCE_BUDGET = 2u, TEST_DEPENDENT_BUDGET = 2u };
static void start(void) {
  check_equal(orm_owner_init(&owner, TEST_REFERENCE_BUDGET,
                            TEST_DEPENDENT_BUDGET), ORM_STATUS_OK);
}

spec("native owner control state") {
  (void)ttest_config__;
  before_each() { memset(&owner, 0, sizeof(owner)); }
  after_each() {
    /* These stack-state tests own no native resources. Always release their
     * synchronization allocation, including a failed assertion's cleanup. */
    salts_mutex_destroy(&owner.mutex);
  }
  it("rejects missing owners and zero budgets before allocating") {
    check_equal(orm_owner_init(NULL, 1u, 1u), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_owner_init(&owner, 0u, 1u), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_owner_init(&owner, 1u, 0u), ORM_STATUS_INVALID_ARGUMENT);
    check_null(owner.mutex);
  }
  it("starts with one reference and no dependent resources") {
    start(); check_equal(owner.references, 1u); check_equal(owner.dependents, 0u);
    check_equal(owner.phase, ORM_OWNER_OPEN); check_not_null(owner.mutex);
  }
  it("bounds references without changing state on exhaustion") {
    start(); check_equal(orm_owner_try_retain(&owner), ORM_STATUS_OK);
    check_equal(orm_owner_try_retain(&owner), ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(owner.references, TEST_REFERENCE_BUDGET);
    check_equal(orm_owner_release_reference(&owner), ORM_OWNER_KEEP);
    check_equal(orm_owner_try_retain(&owner), ORM_STATUS_OK);
  }
  it("bounds dependent admission without leaking a hold") {
    start(); check_equal(orm_owner_admit(&owner), ORM_STATUS_OK);
    check_equal(orm_owner_admit(&owner), ORM_STATUS_OK);
    check_equal(orm_owner_admit(&owner), ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(owner.dependents, TEST_DEPENDENT_BUDGET);
    check_equal(orm_owner_release_dependent(&owner), ORM_OWNER_KEEP);
    check_equal(orm_owner_admit(&owner), ORM_STATUS_OK);
  }
  it("rejects close with dependents without changing the owner") {
    start(); check_equal(orm_owner_admit(&owner), ORM_STATUS_OK);
    orm_owner_action action = ORM_OWNER_FREE_MEMORY;
    check_equal(orm_owner_begin_close(&owner, &action), ORM_STATUS_BUSY);
    check_equal(action, ORM_OWNER_KEEP); check_equal(owner.phase, ORM_OWNER_OPEN);
    check_equal(owner.references, 1u); check_equal(owner.dependents, 1u);
  }
  it("keeps released parents alive until the final dependent leaves") {
    start(); check_equal(orm_owner_admit(&owner), ORM_STATUS_OK);
    check_equal(orm_owner_admit(&owner), ORM_STATUS_OK);
    check_equal(orm_owner_release_reference(&owner), ORM_OWNER_KEEP);
    check_equal(owner.phase, ORM_OWNER_RELEASE_PENDING);
    check_equal(orm_owner_try_retain(&owner), ORM_STATUS_INVALID_STATE);
    check_equal(orm_owner_admit(&owner), ORM_STATUS_INVALID_STATE);
    check_equal(orm_owner_release_dependent(&owner), ORM_OWNER_KEEP);
    check_equal(orm_owner_release_dependent(&owner), ORM_OWNER_CLOSE_RESOURCES);
    check_equal(owner.phase, ORM_OWNER_CLOSING);
    check_equal(orm_owner_finish_close(&owner), ORM_OWNER_FREE_MEMORY);
    orm_owner_dispose(&owner); check_null(owner.mutex);
  }
  it("separates resource close from release of closed handle memory") {
    start(); orm_owner_action action = ORM_OWNER_KEEP;
    check_equal(orm_owner_begin_close(&owner, &action), ORM_STATUS_OK);
    check_equal(action, ORM_OWNER_CLOSE_RESOURCES);
    check_equal(orm_owner_admit(&owner), ORM_STATUS_INVALID_STATE);
    check_equal(orm_owner_finish_close(&owner), ORM_OWNER_KEEP);
    check_equal(owner.phase, ORM_OWNER_CLOSED); check_equal(owner.references, 1u);
    check_equal(orm_owner_begin_close(&owner, &action), ORM_STATUS_OK);
    check_equal(action, ORM_OWNER_KEEP);
    check_equal(orm_owner_release_reference(&owner), ORM_OWNER_FREE_MEMORY);
    orm_owner_dispose(&owner);
  }
  it("claims cleanup exactly once and releases the lock before native work") {
    start(); orm_owner_action action = ORM_OWNER_KEEP;
    check_equal(orm_owner_begin_close(&owner, &action), ORM_STATUS_OK);
    /* A cleanup callback can re-enter close without deadlocking. */
    check_equal(orm_owner_begin_close(&owner, &action), ORM_STATUS_BUSY);
    check_equal(action, ORM_OWNER_KEEP);
    check_equal(orm_owner_finish_close(&owner), ORM_OWNER_KEEP);
    check_equal(orm_owner_begin_close(&owner, &action), ORM_STATUS_OK);
    check_equal(action, ORM_OWNER_KEEP);
  }
  it("defers final memory disposal until in-progress cleanup returns") {
    start(); orm_owner_action action = ORM_OWNER_KEEP;
    check_equal(orm_owner_begin_close(&owner, &action), ORM_STATUS_OK);
    check_equal(orm_owner_release_reference(&owner), ORM_OWNER_KEEP);
    check_equal(owner.phase, ORM_OWNER_CLOSING);
    check_equal(orm_owner_finish_close(&owner), ORM_OWNER_FREE_MEMORY);
    orm_owner_dispose(&owner);
  }
  it("returns a close action rather than running cleanup on final release") {
    start(); check_equal(orm_owner_release_reference(&owner), ORM_OWNER_CLOSE_RESOURCES);
    check_equal(owner.phase, ORM_OWNER_CLOSING);
    check_equal(orm_owner_finish_close(&owner), ORM_OWNER_FREE_MEMORY);
    orm_owner_dispose(&owner);
  }
  it("rejects mutation while dependent leases exist") {
    start(); check_equal(orm_owner_admit(&owner), ORM_STATUS_OK);
    check_equal(orm_owner_begin_write(&owner), ORM_STATUS_BUSY);
    check_equal(orm_owner_release_dependent(&owner), ORM_OWNER_KEEP);
    const orm_status_t status = orm_owner_begin_write(&owner);
    if (status == ORM_STATUS_OK) orm_owner_end_write(&owner);
    check_equal(status, ORM_STATUS_OK);
  }
  it("rejects mutation after close without changing state") {
    start(); orm_owner_action action = ORM_OWNER_KEEP;
    check_equal(orm_owner_begin_close(&owner, &action), ORM_STATUS_OK);
    check_equal(orm_owner_finish_close(&owner), ORM_OWNER_KEEP);
    check_equal(orm_owner_begin_write(&owner), ORM_STATUS_INVALID_STATE);
    check_equal(owner.phase, ORM_OWNER_CLOSED);
  }
}
