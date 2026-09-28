#ifndef ORM_DRIVER_STORAGE_H
#define ORM_DRIVER_STORAGE_H

#include "orm_driver_base.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORM_DRIVER_STORAGE_ABI_VERSION UINT32_C(1)

/*
 * Provider-neutral local durability capabilities.
 *
 * These describe storage facts only. They do not imply Raft, leader, shard,
 * quorum, membership, or distributed-provider policy.
 */
#define ORM_DRIVER_STORAGE_CAP_ATOMIC_STATE_METADATA       (UINT64_C(1) << 0)
#define ORM_DRIVER_STORAGE_CAP_ORDERED_REPLAY_CLASSIFICATION (UINT64_C(1) << 1)
#define ORM_DRIVER_STORAGE_CAP_AMBIGUOUS_COMMIT            (UINT64_C(1) << 2)
#define ORM_DRIVER_STORAGE_CAP_BOUNDED_BATCH               (UINT64_C(1) << 3)
#define ORM_DRIVER_STORAGE_CAP_FILE_BACKED_CHECKPOINT      (UINT64_C(1) << 4)
#define ORM_DRIVER_STORAGE_CAP_STREAMING_CHECKPOINT        (UINT64_C(1) << 5)
#define ORM_DRIVER_STORAGE_CAP_STAGED_RESTORE              (UINT64_C(1) << 6)
#define ORM_DRIVER_STORAGE_CAP_RECONCILE                   (UINT64_C(1) << 7)

#define ORM_DRIVER_STORAGE_CAP_KNOWN_MASK                                      \
  (ORM_DRIVER_STORAGE_CAP_ATOMIC_STATE_METADATA |                              \
   ORM_DRIVER_STORAGE_CAP_ORDERED_REPLAY_CLASSIFICATION |                      \
   ORM_DRIVER_STORAGE_CAP_AMBIGUOUS_COMMIT |                                   \
   ORM_DRIVER_STORAGE_CAP_BOUNDED_BATCH |                                      \
   ORM_DRIVER_STORAGE_CAP_FILE_BACKED_CHECKPOINT |                             \
   ORM_DRIVER_STORAGE_CAP_STREAMING_CHECKPOINT |                               \
   ORM_DRIVER_STORAGE_CAP_STAGED_RESTORE |                                     \
   ORM_DRIVER_STORAGE_CAP_RECONCILE)

/*
 * A limit with this value is real but resolved by the connection/operation
 * configuration rather than fixed by the Driver module. It is not unlimited:
 * the operation must still reject work exceeding its configured finite budget.
 */
#define ORM_DRIVER_STORAGE_LIMIT_CONFIGURED UINT64_MAX

typedef struct orm_driver_storage_capabilities_v1 {
  orm_driver_header_v1 header;
  uint64_t capabilities;
  uint64_t max_batch_operations;
  uint64_t max_batch_bytes;
  uint64_t max_progress_metadata_bytes;
  uint64_t max_checkpoint_chunk_bytes;
  uint64_t max_restore_chunk_bytes;
} orm_driver_storage_capabilities_v1;

#define ORM_DRIVER_STORAGE_CAPABILITIES_NONE_INIT                              \
  {                                                                            \
    { (uint32_t)sizeof(orm_driver_storage_capabilities_v1),                    \
      ORM_DRIVER_STORAGE_ABI_VERSION },                                         \
    UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0),                        \
    UINT64_C(0), UINT64_C(0)                                                    \
  }

static inline int orm_driver_storage_capabilities_valid(
    const orm_driver_storage_capabilities_v1 *storage) {
  uint64_t caps;
  if (storage == NULL ||
      storage->header.struct_size < sizeof(*storage) ||
      storage->header.abi_version != ORM_DRIVER_STORAGE_ABI_VERSION)
    return 0;

  caps = storage->capabilities;
  if ((caps & ~ORM_DRIVER_STORAGE_CAP_KNOWN_MASK) != 0u)
    return 0;

  if ((caps & ORM_DRIVER_STORAGE_CAP_AMBIGUOUS_COMMIT) != 0u &&
      (caps & (ORM_DRIVER_STORAGE_CAP_ORDERED_REPLAY_CLASSIFICATION |
               ORM_DRIVER_STORAGE_CAP_RECONCILE)) !=
          (ORM_DRIVER_STORAGE_CAP_ORDERED_REPLAY_CLASSIFICATION |
           ORM_DRIVER_STORAGE_CAP_RECONCILE))
    return 0;

  if ((caps & ORM_DRIVER_STORAGE_CAP_RECONCILE) != 0u &&
      (caps & ORM_DRIVER_STORAGE_CAP_ORDERED_REPLAY_CLASSIFICATION) == 0u)
    return 0;

  if ((caps & ORM_DRIVER_STORAGE_CAP_BOUNDED_BATCH) != 0u) {
    if (storage->max_batch_operations == 0u ||
        storage->max_batch_bytes == 0u)
      return 0;
  } else if (storage->max_batch_operations != 0u ||
             storage->max_batch_bytes != 0u) {
    return 0;
  }

  if ((caps & ORM_DRIVER_STORAGE_CAP_ATOMIC_STATE_METADATA) != 0u) {
    if (storage->max_progress_metadata_bytes == 0u)
      return 0;
  } else if (storage->max_progress_metadata_bytes != 0u) {
    return 0;
  }

  if ((caps & ORM_DRIVER_STORAGE_CAP_STREAMING_CHECKPOINT) != 0u) {
    if (storage->max_checkpoint_chunk_bytes == 0u)
      return 0;
  } else if (storage->max_checkpoint_chunk_bytes != 0u) {
    return 0;
  }

  if ((caps & ORM_DRIVER_STORAGE_CAP_STAGED_RESTORE) != 0u) {
    if (storage->max_restore_chunk_bytes == 0u)
      return 0;
  } else if (storage->max_restore_chunk_bytes != 0u) {
    return 0;
  }

  return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* ORM_DRIVER_STORAGE_H */
