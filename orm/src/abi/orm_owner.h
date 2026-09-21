#ifndef ORM_OWNER_H
#define ORM_OWNER_H

#ifndef ORM_NATIVE_OWNER_CANDIDATE
#error "Retained owner internals require ORM_NATIVE_OWNER_CANDIDATE"
#endif

#include <orm.h>
#include <salts/thread.h>
#include <stdbool.h>
#include <stdint.h>

/* #28 retained native control state. No registry and no per-lease allocation.
 * The enclosing native object owns this mutex and releases it with its memory.
 * Callers must already own a reference or dependent hold; this cannot validate
 * arbitrary/stale pointers. Budgets are supplied before publication. */
/* Internal aliases keep the owner implementation readable while the public
 * facade owns the stable status taxonomy. */
enum {
  ORM_OWNER_STATUS_COMMIT_UNKNOWN = ORM_STATUS_COMMIT_UNKNOWN,
  ORM_OWNER_STATUS_CLEANUP_FAILED = ORM_STATUS_CLEANUP_FAILED
};

enum {
  ORM_OWNER_DEFAULT_REFERENCES = 256u,
  ORM_OWNER_DEFAULT_DEPENDENTS = 256u
};
typedef enum orm_owner_phase {
  ORM_OWNER_OPEN,
  ORM_OWNER_RELEASE_PENDING,
  ORM_OWNER_CLOSING,
  ORM_OWNER_CLOSED,
  ORM_OWNER_CLOSE_FAILED
} orm_owner_phase;
typedef enum orm_owner_action {
  ORM_OWNER_KEEP,
  ORM_OWNER_CLOSE_RESOURCES,
  ORM_OWNER_FREE_MEMORY
} orm_owner_action;
/* Host-owned policy. Set before admitting any child, then immutable through
 * final cleanup. Error is borrowed only for notify; copy it to keep diagnostics.
 * The host keeps context alive through all delayed notifications.
 * notify must not reenter this connection/runtime. Returning cannot recover a
 * quarantined owner. NULL selects the default fail-fast handler. */
typedef struct orm_owner_cleanup_policy {
  void (*notify)(void *context, const orm_error_t *native_error);
  void *context;
} orm_owner_cleanup_policy;

/* Embedded in an already owned Publisher/transaction. Its parent hold is
 * transferred with a queued request, never recreated at final release.
 * Queue fields are protected by the connection owner mutex. run/finish/context
 * are immutable while queued; finish runs only after unlinking and may free
 * the enclosing object. CANCEL and DESTROY coalesce, including during run. */
enum { ORM_NATIVE_CLEANUP_CANCEL = 1u, ORM_NATIVE_CLEANUP_DESTROY = 2u };
typedef struct orm_native_cleanup {
  struct orm_native_cleanup *next;
  void (*run)(void *, unsigned);
  void (*finish)(void *, unsigned);
  void *context;
  unsigned requested;
  unsigned completed;
  bool queued;
} orm_native_cleanup;

typedef struct orm_owner {
  salts_mutex_t mutex;
  uint32_t references;
  uint32_t dependents;
  uint32_t max_references;
  uint32_t max_dependents;
  orm_owner_phase phase;
} orm_owner;

#ifdef __cplusplus
extern "C" {
#endif

orm_status_t orm_owner_init(orm_owner *, uint32_t max_references,
                            uint32_t max_dependents);
orm_status_t orm_owner_try_retain(orm_owner *);
orm_status_t orm_owner_admit(orm_owner *);
/* Both release operations return an action with the control lock released.
 * CLOSE_RESOURCES is exclusive. The caller performs native cleanup, then calls
 * finish_close; no plugin/native callback may run inside this control lock. */
orm_owner_action orm_owner_release_reference(orm_owner *);
orm_owner_action orm_owner_release_dependent(orm_owner *);
orm_status_t orm_owner_begin_close(orm_owner *, orm_owner_action *);
orm_owner_action orm_owner_finish_close(orm_owner *);
void orm_owner_dispose(orm_owner *);

/* A successful begin_write holds the same mutex used by close/admission.
 * Every successful call must pair with end_write on every return path. */
orm_status_t orm_owner_begin_write(orm_owner *);
void orm_owner_end_write(orm_owner *);

#ifdef __cplusplus
}
#endif

#endif
