#ifndef ORM_OWNER_H
#define ORM_OWNER_H

#ifndef ORM_NATIVE_OWNER_CANDIDATE
#error "Native owner candidate is private to the non-installed test core (#28)"
#endif

#include <orm.h>
#include <salts/thread.h>
#include <stdbool.h>
#include <stdint.h>

/* #28 staged native control state. No registry and no per-lease allocation.
 * The enclosing native object owns this mutex and releases it with its memory.
 * Callers must already own a reference or dependent hold; this cannot validate
 * arbitrary/stale pointers. Budgets are supplied before publication. */
/* Private candidate status; the ABI 5 facade assigns its published taxonomy. */
enum { ORM_OWNER_STATUS_COMMIT_UNKNOWN = 16, ORM_OWNER_STATUS_CLEANUP_FAILED = 17 };

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

/* Candidate declarations only: do not export these through the ABI 4 facade.
 * Publisher/transaction integration and the complete ABI 5 family are tracked
 * separately in #28; these declarations are not an installed public SDK. */
orm_status_t ORM_C_CALL orm_connection_close(orm_connection_t *, orm_error_t *);
void ORM_C_CALL orm_connection_retain(orm_connection_t *);
void ORM_C_CALL orm_connection_release(orm_connection_t *);
orm_status_t ORM_C_CALL orm_query_close(orm_query_t *, orm_error_t *);
void ORM_C_CALL orm_query_retain(orm_query_t *);
void ORM_C_CALL orm_query_release(orm_query_t *);

/* Held handles only. Active transactions require explicit commit/rollback;
 * dependent Publishers/native calls make close BUSY without changing state.
 * Successful close leaves handle memory/parent ownership until final release. */
orm_status_t ORM_C_CALL orm_transaction_close(orm_transaction_t *, orm_error_t *);
void ORM_C_CALL orm_transaction_retain(orm_transaction_t *);
void ORM_C_CALL orm_transaction_release(orm_transaction_t *);

#ifdef __cplusplus
}
#endif

#endif
