# Scheduler Sync Protocol v3 Specification

## Status: DRAFT

**Version:** 3.1
**Date:** 2026-01-28
**Authors:** Claude + Human

## 1. Overview

### 1.1 Purpose

This specification defines a bandwidth-efficient bidirectional synchronization protocol for scheduled jobs between cloud (offboard) and vehicle (onboard) systems.

**Key improvements over v2:**
- Hash-first sync: Exchange checksums/hashes before full data
- Envelope messages: Type-safe message discrimination via protobuf oneof
- Independent execution stream: Immediate execution reporting with optional acknowledgment
- Optimized reconnect: Minimize data when state unchanged

### 1.2 Design Goals

| Goal | Approach |
|------|----------|
| Bandwidth efficiency | Hash-first comparison, only transfer diffs |
| Offline support | Both sides operate independently |
| Automatic resolution | Authority-based conflict resolution |
| Clock independence | Logical clocks (version vectors) |
| Low latency executions | Independent stream, immediate send |
| Simple reconnect | Single checksum comparison first |
| Type safety | Envelope messages with explicit type discrimination |

### 1.3 Bandwidth Comparison

| Scenario | v2 (full sync) | v3 (hash-first) |
|----------|----------------|-----------------|
| 100 jobs, 5 differ | ~30 KB | ~6 KB |
| 10,000 jobs, 50 differ | ~3 MB | ~500 KB |
| Reconnect, no changes | ~30 KB | ~100 bytes |

## 2. Message Types

### 2.1 Overview

All messages use **content_id = 202**. Messages are wrapped in envelope types (`V2C_Envelope` or `C2V_Envelope`) with explicit type discrimination via protobuf `oneof`.

**Vehicle → Cloud (V2C):**

| Message | Purpose | When sent |
|---------|---------|-----------|
| `V2C_Hello` | Announce state checksum | On connect, after applying changes |
| `V2C_HashManifest` | Send all job hashes | When cloud requests |
| `V2C_JobData` | Send full job records | When cloud requests specific jobs |
| `V2C_Executions` | Report execution results | Immediately when jobs complete |

**Cloud → Vehicle (C2V):**

| Message | Purpose | When sent |
|---------|---------|-----------|
| `C2V_RequestHashes` | Request hash manifest | When vehicle checksum unknown |
| `C2V_SyncDelta` | Send jobs + request jobs | After comparing hashes |
| `C2V_ExecutionAck` | Acknowledge received executions | After storing executions |
| `C2V_TriggerJob` | Request immediate job execution | User-initiated trigger |

### 2.2 Protocol Buffers

```protobuf
syntax = "proto3";
package swdv.scheduler_sync_v3;

option cc_enable_arenas = true;

// ============================================================================
// Envelope Messages (for type-safe discrimination)
// ============================================================================

// Vehicle → Cloud envelope
message V2C_Envelope {
    oneof message {
        V2C_Hello hello = 1;
        V2C_HashManifest hash_manifest = 2;
        V2C_JobData job_data = 3;
        V2C_Executions executions = 4;
        V2C_TriggerResponse trigger_response = 5;
    }
}

// Cloud → Vehicle envelope
message C2V_Envelope {
    oneof message {
        C2V_RequestHashes request_hashes = 1;
        C2V_SyncDelta sync_delta = 2;
        C2V_ExecutionAck execution_ack = 3;
        C2V_TriggerJob trigger_job = 4;
    }
}

// ============================================================================
// Vehicle → Cloud Messages
// ============================================================================

// Initial handshake / state announcement
// Sent on connect and after applying any changes
message V2C_Hello {
    string vehicle_id = 1;
    string bridge_instance_id = 2;       // Detect bridge restarts
    uint64 state_checksum = 3;           // xxHash64 of all jobs
    uint64 last_seen_c2v_checksum = 4;   // Last cloud checksum we processed
}

// Job hash manifest (sent when cloud requests)
message V2C_HashManifest {
    string vehicle_id = 1;
    repeated JobHashEntry job_hashes = 2;
    uint64 state_checksum = 3;
}

// Full job data (sent when cloud requests specific jobs)
message V2C_JobData {
    string vehicle_id = 1;
    repeated JobRecord jobs = 2;
    uint64 state_checksum = 3;
}

// Execution results (independent stream)
message V2C_Executions {
    string vehicle_id = 1;
    repeated ExecutionRecord executions = 2;
}

// Response to trigger request
message V2C_TriggerResponse {
    string vehicle_id = 1;
    string job_id = 2;
    string request_id = 3;               // Correlation ID from C2V_TriggerJob
    bool accepted = 4;
    string error_message = 5;            // If !accepted
    uint64 timestamp_ms = 6;
}

// ============================================================================
// Cloud → Vehicle Messages
// ============================================================================

// Request hash manifest (when vehicle checksum is unknown/changed)
message C2V_RequestHashes {
    string vehicle_id = 1;
    uint64 cloud_state_checksum = 2;     // Inform vehicle of cloud state
}

// Sync delta - request specific jobs and/or send jobs
message C2V_SyncDelta {
    string vehicle_id = 1;
    repeated string request_job_ids = 2; // Jobs cloud needs from vehicle
    repeated JobRecord jobs = 3;         // Jobs vehicle needs from cloud
    uint64 state_checksum = 4;
    uint64 last_seen_v2c_checksum = 5;
}

// Acknowledge received executions (allows vehicle to stop retrying)
message C2V_ExecutionAck {
    string vehicle_id = 1;
    repeated string execution_ids = 2;   // Executions cloud has stored
}

// Request immediate job execution (imperative command, not state sync)
message C2V_TriggerJob {
    string vehicle_id = 1;
    string job_id = 2;
    string request_id = 3;               // For correlation with response
    string requester_id = 4;             // Who requested (for audit)
    uint64 timestamp_ms = 5;
    uint64 expires_at_ms = 6;            // Request expires after this (0 = no expiry)
}

// ============================================================================
// Shared Types
// ============================================================================

message JobHashEntry {
    string job_id = 1;
    uint64 content_hash = 2;             // xxHash64 of job content
    JobVersion version = 3;              // For quick dominance check
    bool deleted = 4;                    // Tombstone flag
}

message JobVersion {
    uint64 cloud_seq = 1;
    uint64 vehicle_seq = 2;
}

message JobRecord {
    // Identity
    string job_id = 1;
    JobAuthority authority = 2;
    JobVersion version = 3;

    // Content (included in hash)
    bool deleted = 5;
    string title = 10;
    string service = 11;
    string method = 12;
    string parameters_json = 13;
    uint64 scheduled_time_ms = 14;
    string recurrence_rule = 15;
    uint64 end_time_ms = 16;
    bool paused = 17;
    WakePolicy wake_policy = 18;
    SleepPolicy sleep_policy = 19;
    uint32 wake_lead_time_s = 20;

    // Execution state (vehicle-authoritative, NOT in hash)
    JobStatus status = 25;
    uint64 next_run_time_ms = 26;
    uint64 last_executed_ms = 27;

    // Metadata (NOT in hash)
    uint64 created_at_ms = 30;
    uint64 updated_at_ms = 31;
    uint64 deleted_at_ms = 6;
    string created_by = 32;
}

message ExecutionRecord {
    string execution_id = 1;             // Globally unique (for dedup)
    string job_id = 2;
    uint64 executed_at_ms = 3;
    uint64 duration_ms = 4;
    JobStatus status = 5;                // COMPLETED or FAILED
    string result_json = 6;
    string error_message = 7;
}

enum JobAuthority {
    AUTHORITY_CLOUD = 0;
    AUTHORITY_VEHICLE = 1;
}

enum JobStatus {
    JOB_STATUS_PENDING = 0;
    JOB_STATUS_RUNNING = 1;
    JOB_STATUS_COMPLETED = 2;
    JOB_STATUS_FAILED = 3;
    JOB_STATUS_CANCELLED = 4;
}

enum WakePolicy {
    WAKE_NO_WAKE = 0;
    WAKE_REQUIRED = 1;
}

enum SleepPolicy {
    SLEEP_NORMAL = 0;
    SLEEP_INHIBIT = 1;
}
```

## 3. Sync Flows

### 3.1 Reconnect - Vehicle Unchanged, Cloud Has Changes

Most common case: vehicle reconnects, only cloud has new jobs.

```
VEHICLE                                           CLOUD
   │                                                │
   │ state=0xAAAA                                   │ state=0xBBBB
   │                                                │ last_seen_v2c=0xAAAA
   │                                                │
   │─── V2C_Envelope{hello} ───────────────────────►│
   │    state_checksum: 0xAAAA                      │
   │                                                │
   │                      0xAAAA == last_seen_v2c ✓ │
   │                      Vehicle unchanged!        │
   │                      Just send our changes     │
   │                                                │
   │◄─── C2V_Envelope{sync_delta} ─────────────────│
   │     request_job_ids: []                        │
   │     jobs: [job-X, job-Y]  ← new cloud jobs     │
   │     state_checksum: 0xBBBB                     │
   │                                                │
   │ Apply jobs                                     │
   │ new state=0xBBBB                               │
   │                                                │
   │─── V2C_Envelope{hello} ───────────────────────►│
   │    state_checksum: 0xBBBB                      │
   │    last_seen_c2v: 0xBBBB                       │
   │                                                │
   │                      Checksums match!          │
   │                      QUIESCENT                 │
   │                                                │
   ═══════════════ NO TRAFFIC UNTIL CHANGE ═════════
```

**Bandwidth:** ~200 bytes total (just checksums + delta jobs)

### 3.2 Reconnect - Both Sides Changed

Vehicle and cloud both modified jobs while disconnected.

```
VEHICLE                                           CLOUD
   │                                                │
   │ state=0xCCCC (changed offline)                 │ state=0xBBBB
   │                                                │ last_seen_v2c=0xAAAA
   │                                                │
   │─── V2C_Envelope{hello} ───────────────────────►│
   │    state_checksum: 0xCCCC                      │
   │                                                │
   │                      0xCCCC != last_seen_v2c   │
   │                      Vehicle changed!          │
   │                      Need hash manifest        │
   │                                                │
   │◄─── C2V_Envelope{request_hashes} ─────────────│
   │     cloud_state_checksum: 0xBBBB               │
   │                                                │
   │─── V2C_Envelope{hash_manifest} ───────────────►│
   │    job_hashes: [                               │
   │      {A, 0x111, v:{3,4}},                      │
   │      {B, 0x222, v:{2,5}},  ← vehicle modified  │
   │      {C, 0x333, v:{1,1}},                      │
   │      ... 100 entries                           │
   │    ]                                           │
   │    state_checksum: 0xCCCC                      │
   │                                                │
   │                      Compare each hash:        │
   │                      A: same                   │
   │                      B: differs (veh)          │
   │                      C: same                   │
   │                      X: missing (cloud has)    │
   │                                                │
   │◄─── C2V_Envelope{sync_delta} ─────────────────│
   │     request_job_ids: ["B"]  ← need from veh    │
   │     jobs: [job-X]           ← veh needs        │
   │     state_checksum: 0xDDDD                     │
   │                                                │
   │ Apply job-X                                    │
   │                                                │
   │─── V2C_Envelope{job_data} ────────────────────►│
   │    jobs: [job-B]                               │
   │    state_checksum: 0xEEEE                      │
   │                                                │
   │                      Apply job-B               │
   │                      Cloud state now 0xEEEE   │
   │                                                │
   │◄─── C2V_Envelope{sync_delta} ─────────────────│
   │     request_job_ids: []                        │
   │     jobs: []                                   │
   │     state_checksum: 0xEEEE                     │
   │     last_seen_v2c: 0xEEEE                      │
   │                                                │
   │                      Checksums match!          │
   │                                                │
   ═══════════════════ QUIESCENT ═══════════════════
```

### 3.3 Reconnect - No Changes (Quiescent)

Vehicle reconnects, neither side changed.

```
VEHICLE                                           CLOUD
   │                                                │
   │ state=0xAAAA                                   │ state=0xAAAA
   │                                                │ last_seen_v2c=0xAAAA
   │                                                │
   │─── V2C_Envelope{hello} ───────────────────────►│
   │    state_checksum: 0xAAAA                      │
   │    last_seen_c2v: 0xAAAA                       │
   │                                                │
   │                      0xAAAA == last_seen_v2c ✓ │
   │                      0xAAAA == our state ✓     │
   │                      Already in sync!          │
   │                                                │
   │◄─── C2V_Envelope{sync_delta} ─────────────────│
   │     request_job_ids: []                        │
   │     jobs: []                                   │
   │     state_checksum: 0xAAAA                     │
   │     last_seen_v2c: 0xAAAA                      │
   │                                                │
   ═══════════════════ QUIESCENT ═══════════════════
```

**Bandwidth:** ~100 bytes (just checksums)

### 3.4 Execution Reporting (With Acknowledgment)

Executions are sent immediately. Cloud acknowledges to stop retries.

```
VEHICLE                                           CLOUD
   │                                                │
   │ Job X completes                                │
   │                                                │
   │─── V2C_Envelope{executions} ──────────────────►│
   │    executions: [{                              │
   │      execution_id: "exec-123",                 │
   │      job_id: "job-X",                          │
   │      status: COMPLETED,                        │
   │      duration_ms: 1500,                        │
   │      result_json: "{...}"                      │
   │    }]                                          │
   │                                                │
   │                      Store execution           │
   │                      (dedup by execution_id)   │
   │                                                │
   │◄─── C2V_Envelope{execution_ack} ──────────────│
   │     execution_ids: ["exec-123"]                │
   │                                                │
   │ Remove from retry queue                        │
   │                                                │
```

**Key properties:**
- Sent immediately when job completes
- Cloud deduplicates by `execution_id`
- Independent of job sync state
- Vehicle queues for retry until acknowledged
- Can batch multiple executions in one message
- Acknowledgment is optional (vehicle times out after N retries)

### 3.5 Job Trigger (Imperative Command)

Trigger is the only imperative command - everything else is state sync.

```
VEHICLE                                           CLOUD
   │                                                │
   │                      User clicks "Run Now"     │
   │                                                │
   │◄─── C2V_Envelope{trigger_job} ────────────────│
   │     job_id: "job-X"                            │
   │     request_id: "req-456"                      │
   │     requester_id: "dashboard-user"             │
   │                                                │
   │ Execute job immediately                        │
   │                                                │
   │─── V2C_Envelope{trigger_response} ────────────►│
   │    job_id: "job-X"                             │
   │    request_id: "req-456"                       │
   │    accepted: true                              │
   │                                                │
   │ ... job runs ...                               │
   │                                                │
   │─── V2C_Envelope{executions} ──────────────────►│
   │    executions: [{...}]                         │
   │                                                │
```

## 4. Cloud Decision Logic

### 4.1 On Receiving V2C_Hello

```python
def handle_v2c_hello(msg):
    vehicle_checksum = msg.state_checksum
    known_checksum = get_last_seen_v2c_checksum(msg.vehicle_id)
    our_checksum = get_cloud_state_checksum(msg.vehicle_id)

    if vehicle_checksum == known_checksum:
        # We know vehicle's state - just send our changes
        if vehicle_checksum == our_checksum:
            # Already in sync - send empty delta (confirm quiescent)
            send_c2v_envelope(sync_delta=C2V_SyncDelta(
                request_job_ids=[],
                jobs=[],
                state_checksum=our_checksum
            ))
        else:
            # Vehicle unchanged, but we have changes
            changed_jobs = get_jobs_changed_since(known_checksum)
            send_c2v_envelope(sync_delta=C2V_SyncDelta(
                request_job_ids=[],
                jobs=changed_jobs,
                state_checksum=our_checksum
            ))
    else:
        # Vehicle changed - we need their hash manifest
        send_c2v_envelope(request_hashes=C2V_RequestHashes(
            cloud_state_checksum=our_checksum
        ))
```

### 4.2 On Receiving V2C_HashManifest

```python
def handle_v2c_hash_manifest(msg):
    request_jobs = []
    send_jobs = []

    vehicle_hashes = {h.job_id: h for h in msg.job_hashes}
    cloud_jobs = get_all_jobs(msg.vehicle_id)

    # Check each cloud job
    for job in cloud_jobs:
        if job.job_id in vehicle_hashes:
            vh = vehicle_hashes[job.job_id]
            if vh.content_hash != job.content_hash:
                # Content differs - compare versions
                if vehicle_dominates(vh.version, job.version):
                    request_jobs.append(job.job_id)
                elif cloud_dominates(job.version, vh.version):
                    send_jobs.append(job)
                else:
                    # Conflict - resolve by authority
                    winner = resolve_conflict(job, vh)
                    if winner == 'vehicle':
                        request_jobs.append(job.job_id)
                    else:
                        send_jobs.append(job)
        else:
            # Cloud has job vehicle doesn't
            send_jobs.append(job)

    # Check for jobs vehicle has that cloud doesn't
    for job_id in vehicle_hashes:
        if job_id not in cloud_jobs:
            request_jobs.append(job_id)

    send_c2v_envelope(sync_delta=C2V_SyncDelta(
        request_job_ids=request_jobs,
        jobs=send_jobs,
        state_checksum=compute_checksum()
    ))
```

### 4.3 On Receiving V2C_Executions

```python
def handle_v2c_executions(msg):
    acked_ids = []
    for exec in msg.executions:
        # Deduplicate by execution_id
        if not execution_exists(exec.execution_id):
            store_execution(msg.vehicle_id, exec)
        acked_ids.append(exec.execution_id)

    # Acknowledge all (including duplicates - idempotent)
    send_c2v_envelope(execution_ack=C2V_ExecutionAck(
        execution_ids=acked_ids
    ))
```

## 5. Version Vectors & Conflict Resolution

### 5.1 Dominance

Version A **dominates** B if:
```
A.cloud_seq >= B.cloud_seq AND
A.vehicle_seq >= B.vehicle_seq AND
(A.cloud_seq > B.cloud_seq OR A.vehicle_seq > B.vehicle_seq)
```

### 5.2 Conflict Resolution

When neither dominates:
1. Check job's `authority` field
2. `AUTHORITY_CLOUD` → cloud content wins
3. `AUTHORITY_VEHICLE` → vehicle content wins
4. Merged version: `{max(cloud_seq)+1, max(vehicle_seq)}` (cloud resolving)

## 6. Checksum Computation

### 6.1 State Checksum

**Algorithm:** xxHash64

**Input:** All jobs sorted by `job_id`, each job contributing:
- job_id, authority, version.cloud_seq, version.vehicle_seq, deleted
- title, service, method, parameters_json
- scheduled_time_ms, recurrence_rule, end_time_ms
- paused, wake_policy, sleep_policy, wake_lead_time_s

**Excluded:** status, next_run_time_ms, created_at_ms, updated_at_ms, deleted_at_ms

### 6.2 Job Content Hash

Same fields as state checksum, but for a single job.

## 7. Tombstone Deletion

- Tombstones are `JobRecord{deleted=true}`
- Included in hash manifest with `deleted=true`
- GC after confirmed + retention period (7 days)

## 8. Execution Records

### 8.1 Properties

- **Append-only:** Immutable facts, never modified
- **Independent:** Not part of job sync, own message type in envelope
- **Immediate:** Sent as soon as job completes
- **Deduplicated:** Cloud stores by `execution_id`
- **Acknowledged:** Cloud sends `C2V_ExecutionAck` to stop retries

### 8.2 Vehicle Behavior

```python
def on_job_completed(job, result):
    execution = ExecutionRecord(
        execution_id=generate_uuid(),
        job_id=job.job_id,
        executed_at_ms=now_ms(),
        duration_ms=result.duration,
        status=result.status,
        result_json=result.output,
        error_message=result.error
    )

    # Send immediately
    send_v2c_envelope(executions=V2C_Executions(
        executions=[execution]
    ))

    # Queue for retry until acknowledged
    pending_executions[execution.execution_id] = execution

def on_execution_ack(msg):
    for exec_id in msg.execution_ids:
        pending_executions.pop(exec_id, None)

def retry_pending_executions():
    """Called periodically (e.g., every 30s)"""
    if pending_executions:
        send_v2c_envelope(executions=V2C_Executions(
            executions=list(pending_executions.values())
        ))
```

### 8.3 Cloud Behavior

```python
def handle_v2c_executions(msg):
    acked_ids = []
    for exec in msg.executions:
        # Deduplicate by execution_id - idempotent
        if not execution_exists(exec.execution_id):
            store_execution(msg.vehicle_id, exec)
        acked_ids.append(exec.execution_id)

    # Always acknowledge (idempotent)
    send_c2v_envelope(execution_ack=C2V_ExecutionAck(
        execution_ids=acked_ids
    ))
```

## 9. Implementation Notes

### 9.1 Message Handling

All messages use envelope types with protobuf `oneof` for type-safe discrimination:

```cpp
void handle_v2c_message(const bytes& payload) {
    V2C_Envelope envelope;
    if (!envelope.ParseFromString(payload)) {
        LOG(WARNING) << "Failed to parse V2C_Envelope";
        return;
    }

    switch (envelope.message_case()) {
        case V2C_Envelope::kHello:
            handle_hello(envelope.hello());
            break;
        case V2C_Envelope::kHashManifest:
            handle_hash_manifest(envelope.hash_manifest());
            break;
        case V2C_Envelope::kJobData:
            handle_job_data(envelope.job_data());
            break;
        case V2C_Envelope::kExecutions:
            handle_executions(envelope.executions());
            break;
        case V2C_Envelope::kTriggerResponse:
            handle_trigger_response(envelope.trigger_response());
            break;
        case V2C_Envelope::MESSAGE_NOT_SET:
            LOG(WARNING) << "Empty V2C_Envelope";
            break;
    }
}

void handle_c2v_message(const bytes& payload) {
    C2V_Envelope envelope;
    if (!envelope.ParseFromString(payload)) {
        LOG(WARNING) << "Failed to parse C2V_Envelope";
        return;
    }

    switch (envelope.message_case()) {
        case C2V_Envelope::kRequestHashes:
            handle_request_hashes(envelope.request_hashes());
            break;
        case C2V_Envelope::kSyncDelta:
            handle_sync_delta(envelope.sync_delta());
            break;
        case C2V_Envelope::kExecutionAck:
            handle_execution_ack(envelope.execution_ack());
            break;
        case C2V_Envelope::kTriggerJob:
            handle_trigger_job(envelope.trigger_job());
            break;
        case C2V_Envelope::MESSAGE_NOT_SET:
            LOG(WARNING) << "Empty C2V_Envelope";
            break;
    }
}
```

### 9.2 Vehicle State Machine

```
                    ┌──────────────────────────────────────┐
                    │                                      │
                    ▼                                      │
            ┌───────────────┐                              │
    ───────►│   SEND_HELLO  │                              │
   connect  └───────┬───────┘                              │
                    │                                      │
                    │ send V2C_Envelope{hello}             │
                    ▼                                      │
            ┌───────────────┐                              │
            │ WAIT_RESPONSE │                              │
            └───────┬───────┘                              │
                    │                                      │
        ┌───────────┼───────────┐                          │
        │           │           │                          │
        ▼           ▼           ▼                          │
   request_     sync_delta  sync_delta                     │
   hashes       (has jobs)  (empty)                        │
        │           │           │                          │
        ▼           │           ▼                          │
┌───────────────┐   │   ┌───────────────┐                  │
│ SEND_MANIFEST │   │   │  QUIESCENT    │◄─────────────────┤
└───────┬───────┘   │   └───────┬───────┘                  │
        │           │           │                          │
        │           │           │ local change             │
        │           ▼           │                          │
        │   ┌───────────────┐   │                          │
        │   │ APPLY_CHANGES │───┘                          │
        │   └───────┬───────┘                              │
        │           │                                      │
        │           │ if request_job_ids not empty         │
        │           ▼                                      │
        │   ┌───────────────┐                              │
        └──►│ SEND_JOBDATA  │──────────────────────────────┘
            └───────────────┘
```

### 9.3 Execution Retry State Machine

```
                ┌────────────────────────────────────┐
                │                                    │
                ▼                                    │
        ┌───────────────┐                            │
        │    PENDING    │ ◄─── job completes         │
        └───────┬───────┘                            │
                │                                    │
                │ send V2C_Envelope{executions}      │
                ▼                                    │
        ┌───────────────┐                            │
        │ AWAIT_ACK     │────── timeout ─────────────┘
        └───────┬───────┘       (retry)
                │
                │ C2V_ExecutionAck received
                ▼
        ┌───────────────┐
        │    DONE       │
        └───────────────┘
```

## 10. Migration from v2

### 10.1 Compatibility

v3 is NOT backward compatible with v2. Both sides must upgrade.

### 10.2 Migration Steps

1. Deploy cloud with v3 support (accept both v2 and v3)
2. Deploy vehicles with v3
3. Vehicles send `V2C_Envelope{hello}` (v3 format)
4. Cloud responds with v3 messages
5. After all vehicles upgraded, remove v2 support

### 10.3 Version Detection

Cloud can detect v2 vs v3 by message format:
- v2: Raw `V2C_SyncMessage` (no envelope wrapper)
- v3: `V2C_Envelope` with `oneof` discriminator

```cpp
bool is_v3_message(const bytes& payload) {
    V2C_Envelope envelope;
    return envelope.ParseFromString(payload) &&
           envelope.message_case() != V2C_Envelope::MESSAGE_NOT_SET;
}
```

---

## Appendix A: Comparison with Discovery Sync

| Aspect | Discovery Sync | Scheduler Sync v3 |
|--------|----------------|-------------------|
| Data type | Static schemas | Dynamic jobs |
| Hash level | Schema content | Job content |
| Conflict | N/A (immutable) | Version vectors + authority |
| Direction | Mostly V2C | Bidirectional |
| First message | Hash list | Single checksum |
| Request granularity | By hash | By job_id |
| Message wrapper | None | Envelope with oneof |

## Appendix B: Test Scenarios

| Scenario | Messages | Bandwidth |
|----------|----------|-----------|
| Reconnect, no change | Hello → SyncDelta(empty) | ~100 B |
| Reconnect, cloud +5 jobs | Hello → SyncDelta(5 jobs) | ~2 KB |
| Reconnect, both +5 jobs | Hello → RequestHashes → Manifest → SyncDelta → JobData | ~8 KB |
| 10K jobs, 50 differ | Hello → RequestHashes → Manifest → SyncDelta → JobData | ~500 KB |
| Execution report | Executions → ExecutionAck | ~250 B |
| Trigger job | TriggerJob → TriggerResponse → Executions | ~400 B |

## Appendix C: Changes from v3.0 to v3.1

| Change | v3.0 | v3.1 |
|--------|------|------|
| Message discrimination | Try-parse each type | Envelope with oneof |
| Execution ack | None (fire-and-forget) | `C2V_ExecutionAck` |
| Trigger command | Not specified | `C2V_TriggerJob`, `V2C_TriggerResponse` |
| Type safety | Runtime parsing | Compile-time oneof |
