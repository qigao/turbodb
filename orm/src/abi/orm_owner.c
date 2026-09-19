#include "orm_owner.h"

#include <stdlib.h>
#include <string.h>

static bool owner_valid(const orm_owner *owner) {
  return owner != NULL && owner->mutex != NULL;
}

orm_status_t orm_owner_init(orm_owner *owner, uint32_t max_references,
                            uint32_t max_dependents) {
  if (owner == NULL || max_references == 0u || max_dependents == 0u)
    return ORM_STATUS_INVALID_ARGUMENT;
  if (owner->mutex != NULL)
    return ORM_STATUS_INVALID_STATE;
  memset(owner, 0, sizeof(*owner));
  salts_mutex_init(&owner->mutex);
  if (owner->mutex == NULL)
    return ORM_STATUS_OUT_OF_MEMORY;
  owner->references = 1u;
  owner->max_references = max_references;
  owner->max_dependents = max_dependents;
  owner->phase = ORM_OWNER_OPEN;
  return ORM_STATUS_OK;
}

orm_status_t orm_owner_try_retain(orm_owner *owner) {
  orm_status_t status = ORM_STATUS_OK;
  if (!owner_valid(owner)) return ORM_STATUS_INVALID_ARGUMENT;
  salts_mutex_lock(&owner->mutex);
  if (owner->references == 0u || owner->phase == ORM_OWNER_CLOSING)
    status = ORM_STATUS_INVALID_STATE;
  else if (owner->references >= owner->max_references)
    status = ORM_STATUS_LIMIT_EXCEEDED;
  else
    ++owner->references;
  salts_mutex_unlock(&owner->mutex);
  return status;
}

orm_status_t orm_owner_admit(orm_owner *owner) {
  orm_status_t status = ORM_STATUS_OK;
  if (!owner_valid(owner)) return ORM_STATUS_INVALID_ARGUMENT;
  salts_mutex_lock(&owner->mutex);
  if (owner->phase != ORM_OWNER_OPEN || owner->references == 0u)
    status = ORM_STATUS_INVALID_STATE;
  else if (owner->dependents >= owner->max_dependents)
    status = ORM_STATUS_LIMIT_EXCEEDED;
  else
    ++owner->dependents;
  salts_mutex_unlock(&owner->mutex);
  return status;
}

/* Called only under the lock. Cleanup is claimed here, never executed here. */
static orm_owner_action owner_after_release(orm_owner *owner) {
  if (owner->references != 0u) return ORM_OWNER_KEEP;
  if (owner->dependents != 0u) {
    if (owner->phase == ORM_OWNER_OPEN)
      owner->phase = ORM_OWNER_RELEASE_PENDING;
    return ORM_OWNER_KEEP;
  }
  if (owner->phase == ORM_OWNER_CLOSED) return ORM_OWNER_FREE_MEMORY;
  if (owner->phase == ORM_OWNER_CLOSING) return ORM_OWNER_KEEP;
  owner->phase = ORM_OWNER_CLOSING;
  return ORM_OWNER_CLOSE_RESOURCES;
}

static orm_owner_action owner_release(orm_owner *owner, bool dependent) {
  orm_owner_action action;
  if (!owner_valid(owner)) abort();
  salts_mutex_lock(&owner->mutex);
  uint32_t *count = dependent ? &owner->dependents : &owner->references;
  if (*count == 0u) abort(); /* Invalid/double release is a caller contract bug. */
  --*count;
  action = owner_after_release(owner);
  salts_mutex_unlock(&owner->mutex);
  return action;
}

orm_owner_action orm_owner_release_reference(orm_owner *owner) {
  return owner_release(owner, false);
}

orm_owner_action orm_owner_release_dependent(orm_owner *owner) {
  return owner_release(owner, true);
}

orm_status_t orm_owner_begin_close(orm_owner *owner, orm_owner_action *action) {
  orm_status_t status = ORM_STATUS_OK;
  if (action != NULL) *action = ORM_OWNER_KEEP;
  if (!owner_valid(owner) || action == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  salts_mutex_lock(&owner->mutex);
  if (owner->phase == ORM_OWNER_CLOSED) {
    /* A held, already-closed handle remains a legal idempotent close target. */
  } else if (owner->dependents != 0u || owner->phase == ORM_OWNER_CLOSING) {
    status = ORM_STATUS_BUSY;
  } else if (owner->references == 0u || owner->phase != ORM_OWNER_OPEN) {
    status = ORM_STATUS_INVALID_STATE;
  } else {
    owner->phase = ORM_OWNER_CLOSING;
    *action = ORM_OWNER_CLOSE_RESOURCES;
  }
  salts_mutex_unlock(&owner->mutex);
  return status;
}

orm_owner_action orm_owner_finish_close(orm_owner *owner) {
  orm_owner_action action;
  if (!owner_valid(owner)) abort();
  salts_mutex_lock(&owner->mutex);
  if (owner->phase != ORM_OWNER_CLOSING || owner->dependents != 0u) abort();
  owner->phase = ORM_OWNER_CLOSED;
  action = owner->references == 0u ? ORM_OWNER_FREE_MEMORY : ORM_OWNER_KEEP;
  salts_mutex_unlock(&owner->mutex);
  return action;
}

void orm_owner_dispose(orm_owner *owner) {
  /* No user may touch this memory after the final action; no live mutex waiter
   * is legal without its own reference or dependent hold. */
  if (!owner_valid(owner) || owner->references != 0u || owner->dependents != 0u ||
      owner->phase != ORM_OWNER_CLOSED) abort();
  salts_mutex_destroy(&owner->mutex);
}

orm_status_t orm_owner_begin_write(orm_owner *owner) {
  orm_status_t status = ORM_STATUS_OK;
  if (!owner_valid(owner)) return ORM_STATUS_INVALID_ARGUMENT;
  salts_mutex_lock(&owner->mutex);
  if (owner->phase != ORM_OWNER_OPEN || owner->references == 0u)
    status = ORM_STATUS_INVALID_STATE;
  else if (owner->dependents != 0u)
    status = ORM_STATUS_BUSY;
  if (status != ORM_STATUS_OK) salts_mutex_unlock(&owner->mutex);
  return status;
}

void orm_owner_end_write(orm_owner *owner) {
  if (!owner_valid(owner)) abort();
  salts_mutex_unlock(&owner->mutex);
}
