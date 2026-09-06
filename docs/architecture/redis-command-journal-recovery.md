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

## Constraints

All metadata, journal, identity, and outbox keys use one equal, non-empty
Redis Cluster hash tag. Reconciliation and application are serialized by the
same Redis key slot. Stream consumers must verify that an event index is no
greater than `metadata.applied_index` before settlement.

The caller configures the borrowed `redis_cflow_connection` with a bounded
`max_command_bytes` budget large enough for the EVAL text, RESP framing, and
the complete request payload. TurboDB fails before sending when that budget is
insufficient; it does not split a Raft batch or select an unbounded fallback.
