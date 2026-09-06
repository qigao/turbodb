# Redis Command Journal Recovery

## Decision

`metadata.applied_index` is the only durable fact that a Raft command range
has become application state. Journal and identity data above that index are
preparation. Stream events are derived delivery records, not a Raft commit
acknowledgement.

Each outbox event uses the deterministic Redis Stream ID `<raft-index>-0`.
Before publishing, the Lua batch script reads that exact Stream ID. An absent
event is appended; an existing event must have exactly the same four ordered
fields (`index`, `term`, `command_id`, `payload`) or the operation reports
`CONFLICT`. Thus rerunning a partially prepared batch cannot duplicate its
outbox event.

## Reconciliation Contract

`redis_lua_apply_batch_reconcile_open` performs no Redis writes. It validates
the same request and returns one receipt:

| Receipt | Meaning | Caller action |
| --- | --- | --- |
| `REPLAYED` | Every requested entry is committed and byte-identical. | Treat as success. |
| `PENDING` | The committed prefix is byte-identical and the next requested entry follows metadata. | Retry the exact batch once through `redis_lua_apply_batch_open`. |
| `GAP` | The requested first index cannot follow the metadata marker. | Rebuild from the Raft WAL; do not retry. |
| `CONFLICT` | A committed identity or payload differs. | Fault the state machine; do not retry. |
| `COMMIT_UNKNOWN` or transport error | The read result is itself uncertain. | Reconnect and reconcile again; do not apply blindly. |

The caller owns retry policy. TurboDB guarantees only that a `PENDING` result
permits the identical request to be retried without changing the journal,
identity, or outbox facts for already prepared entries.

## Snapshot-aware Journal Compaction

`redis_lua_apply_batch_compact_open` removes a single explicitly bounded,
inclusive journal range after the caller has made the corresponding Raft
snapshot durable. The request supplies `first_index`, `last_index`,
`snapshot_index`, and `snapshot_term`; its four metadata, journal, identity,
and outbox keys follow the same Cluster hash-tag rule as batch apply. A range
cannot exceed `REDIS_LUA_APPLY_BATCH_MAX_RECORDS`, is not split by TurboDB,
and must begin at `metadata.journal_floor + 1`.

`metadata` is the sole retention fact. Before the first delete, the Lua
transaction verifies that `metadata.applied_index >= snapshot_index`, that the
identity at `snapshot_index` begins with the exact snapshot term, and that
every journal and identity field in the range exists. It then deletes journal
fields, deletes identity fields, and writes these metadata fields last:

| Field | Meaning |
| --- | --- |
| `journal_floor` | Inclusive Raft index through which journal facts were removed. |
| `journal_compaction_first_index` | First index of the most recently completed compaction request. |
| `journal_compaction_snapshot_index` | Durable snapshot index that authorized that request. |
| `journal_compaction_snapshot_term` | Durable snapshot term that authorized that request. |

This ordering makes the metadata tuple the compaction commit marker. A lost
reply must be handled by `redis_lua_apply_batch_compact_reconcile_open`:

| Receipt | Meaning | Caller action |
| --- | --- | --- |
| `REPLAYED` | `journal_floor` and all three persisted request facts exactly match. | Treat as compacted. |
| `PENDING` | The preceding floor, applied marker, snapshot identity, and full source range are still valid. | Retry the identical request once through `redis_lua_apply_batch_compact_open`. |
| `GAP` | The applied marker, retention floor, or source hashes cannot establish the requested contiguous range. | Stop and rebuild/recover from the Raft fact source. |
| `CONFLICT` | The snapshot identity, persisted completion tuple, or Redis type is incompatible. | Fault the state machine; do not retry. |
| `COMMIT_UNKNOWN` or transport error | The caller cannot yet know whether the script committed. | Reconnect and reconcile again; do not compact blindly. |

The Stream outbox is deliberately not a journal-retention structure. Neither
compaction script calls `XDEL`, `XTRIM`, or another Stream mutation; its
consumer group owns delivery retention and settlement. TurboRaft must quiesce
apply/write activity for the compacted prefix, retain the durable snapshot
until it observes `APPLIED` or `REPLAYED`, and issue each successive bounded
range with the latest `journal_floor`.

## Constraints

All metadata, journal, identity, and outbox keys use one equal, non-empty
Redis Cluster hash tag. Reconciliation and application are serialized by the
same Redis key slot. Stream consumers must verify that an event index is no
greater than `metadata.applied_index` before settlement.

The caller configures the borrowed `redis_cflow_connection` with a bounded
`max_command_bytes` budget large enough for the EVAL text, RESP framing, and
the complete request payload. TurboDB fails before sending when that budget is
insufficient; it does not split a Raft batch or select an unbounded fallback.
