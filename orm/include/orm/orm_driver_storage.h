#ifndef ORM_DRIVER_STORAGE_H
#define ORM_DRIVER_STORAGE_H

#include "orm_driver_base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ORM_DRIVER_STORAGE_CAP_ATOMIC_STATE_METADATA \
  (UINT64_C(1) << 0)
#define ORM_DRIVER_STORAGE_CAP_ORDERED_REPLAY_CLASSIFICATION \
  (UINT64_C(1) << 1)
#define ORM_DRIVER_STORAGE_CAP_AMBIGUOUS_COMMIT \
  (UINT64_C(1) << 2)
#define ORM_DRIVER_STORAGE_CAP_BOUNDED_BATCH \
  (UINT64_C(1) << 3)
#define ORM_DRIVER_STORAGE_CAP_FILE_BACKED_CHECKPOINT \
  (UINT64_C(1) << 4)
#define ORM_DRIVER_STORAGE_CAP_STREAMING_CHECKPOINT \
  (UINT64_C(1) << 5)
#define ORM_DRIVER_STORAGE_CAP_STAGED_RESTORE \
  (UINT64_C(1) << 6)
#define ORM_DRIVER_STORAGE_CAP_RECONCILE \
  (UINT64_C(1) << 7)
#define ORM_DRIVER_STORAGE_CAP_KNOWN_MASK \
  (ORM_DRIVER_STORAGE_CAP_ATOMIC_STATE_METADATA | \
   ORM_DRIVER_STORAGE_CAP_ORDERED_REPLAY_CLASSIFICATION | \
   ORM_DRIVER_STORAGE_CAP_AMBIGUOUS_COMMIT | \
   ORM_DRIVER_STORAGE_CAP_BOUNDED_BATCH | \
   ORM_DRIVER_STORAGE_CAP_FILE_BACKED_CHECKPOINT | \
   ORM_DRIVER_STORAGE_CAP_STREAMING_CHECKPOINT | \
   ORM_DRIVER_STORAGE_CAP_STAGED_RESTORE | \
   ORM_DRIVER_STORAGE_CAP_RECONCILE)

/*
 * Provider-neutral local storage facts. This is an optional tail table of
 * orm_driver_api_v1; absence means no external replicated-state storage
 * capabilities are advertised.
 *
 * These flags describe local durability only. They do not define or expose
 * leader, term, quorum, group, membership, shard, or replication semantics.
 */
typedef struct orm_driver_storage_capabilities_v1 {
  orm_driver_header_v1 header;
  uint64_t capabilities;
  uint64_t max_batch_operations;
  uint64_t max_batch_bytes;
  uint64_t max_progress_metadata_bytes;
  uint64_t max_checkpoint_chunk_bytes;
  uint64_t max_restore_chunk_bytes;
} orm_driver_storage_capabilities_v1;

#ifdef __cplusplus
}
#endif

#endif
