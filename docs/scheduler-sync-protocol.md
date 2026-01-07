# Scheduler Sync Protocol Specification

## Overview

The Scheduler Sync Protocol enables bidirectional synchronization of scheduled jobs between vehicle and cloud. Unlike Discovery (static schemas shared across fleet), Scheduler handles dynamic per-vehicle job state.

## Design Principles

1. **Delta sync**: Only changed jobs transmitted, not full state
2. **Hash-based change detection**: Job content hash triggers sync
3. **Bidirectional**: Vehicle syncs state up, cloud pushes commands down
4. **Idempotent**: Same message can be processed multiple times safely
5. **Ordered**: Events processed in sequence per vehicle

## Content ID

All scheduler messages use **content_id=202**.

| Direction | Topic Pattern | Payload |
|-----------|---------------|---------|
| v2c | `v2c/{vehicle_id}/202` | `sync_message_t` |
| c2v | `c2v/{vehicle_id}/202` | `scheduler_command_t` |

## Vehicle-to-Cloud: State Sync

### Message Types

```protobuf
message sync_message_t {
    string vehicle_id = 1;
    string bridge_instance_id = 2;
    repeated sync_event_t events = 3;
    uint32 active_jobs_count = 4;
    uint32 state_checksum = 5;      // CRC32 of sorted job hashes
}

message sync_event_t {
    sync_event_type_t event_type = 1;
    uint64 sequence_number = 2;
    uint64 timestamp_ns = 3;
    string job_id = 4;
    job_info_t job_info = 5;        // For CREATE/UPDATE
    execution_result_t result = 6;  // For EXECUTED
}

enum sync_event_type_t {
    FULL_SYNC = 0;      // Complete state snapshot
    JOB_CREATED = 1;    // New job added
    JOB_UPDATED = 2;    // Existing job modified
    JOB_DELETED = 3;    // Job removed
    JOB_EXECUTED = 4;   // Job completed/failed
    HEARTBEAT = 5;      // Liveness signal
}
```

### Sync Flow

```
VEHICLE                                         CLOUD
   │                                              │
   │  (startup)                                   │
   │                                              │
   ├──── FULL_SYNC ───────────────────────────────▶
   │     [job1, job2, job3]                       │
   │     checksum=0xABCD                          │
   │                                              │
   │  (job changes detected via hash)             │
   │                                              │
   ├──── JOB_UPDATED ─────────────────────────────▶
   │     job_id=job2, new_state                   │
   │     checksum=0xDEF0                          │
   │                                              │
   ├──── JOB_EXECUTED ────────────────────────────▶
   │     job_id=job1, status=COMPLETED            │
   │     result="success"                         │
   │                                              │
   │  (30s idle)                                  │
   │                                              │
   ├──── HEARTBEAT ───────────────────────────────▶
   │     active_jobs_count=2                      │
   │     checksum=0x1234                          │
   │                                              │
```

### Change Detection

Each job has a content hash:

```cpp
uint64_t ComputeHash(const job_info_t& job) {
    // Hash of: job_id, title, service, method, parameters,
    //          scheduled_time, recurrence_rule, next_run_time,
    //          status, updated_at_ms
    return XXH64(serialized_fields);
}
```

The sync bridge:
1. Polls scheduler every 1 second
2. Computes hash of each job
3. Compares against cached hash
4. Generates event only if hash changed

### State Checksum

Overall state checksum for sync verification:

```cpp
uint32_t ComputeStateChecksum(const std::map<string, uint64_t>& job_hashes) {
    // CRC32 of sorted (job_id, hash) pairs
    // Enables cloud to verify complete sync without full state transfer
}
```

### Event Batching

Events are batched to reduce message overhead:

```
Poll → Event Queue → [100ms window] → Flush → Single sync_message_t
```

## Cloud-to-Vehicle: Commands

### Message Types

```protobuf
message scheduler_command_t {
    string command_id = 1;          // Unique ID for tracking
    uint64 timestamp_ns = 2;
    string requester_id = 3;        // API caller identity
    command_type_t type = 4;

    oneof payload {
        job_definition_t create_job = 10;
        job_update_t update_job = 11;
        string delete_job_id = 12;
        string pause_job_id = 13;
        string resume_job_id = 14;
        string trigger_job_id = 15;
    }
}

message scheduler_command_ack_t {
    string command_id = 1;
    bool success = 2;
    string error_message = 3;
    string job_id = 4;              // Created job ID
    uint64 timestamp_ns = 5;
}

enum command_type_t {
    COMMAND_CREATE_JOB = 0;
    COMMAND_UPDATE_JOB = 1;
    COMMAND_DELETE_JOB = 2;
    COMMAND_PAUSE_JOB = 3;
    COMMAND_RESUME_JOB = 4;
    COMMAND_TRIGGER_JOB = 5;
}
```

### Command Flow

```
CLOUD                                           VEHICLE
   │                                              │
   ├──── CREATE_JOB ──────────────────────────────▶
   │     command_id=cmd-abc                       │
   │     job: {service, method, schedule}         │
   │                                              │
   │                                              │
   │     Vehicle executes via Scheduler gRPC      │
   │                                              │
   ◀──── ACK ─────────────────────────────────────┤
   │     command_id=cmd-abc                       │
   │     success=true                             │
   │     job_id=job-xyz                           │
   │                                              │
   │                                              │
   │     Normal sync picks up new job             │
   │                                              │
   ◀──── JOB_CREATED ─────────────────────────────┤
   │     job_id=job-xyz                           │
   │                                              │
```

### Offline Command Delivery

Commands to offline vehicles are queued by MQTT broker:

```
Cloud publishes → MQTT Broker queues (QoS 1) → Vehicle reconnects → Delivers
```

Requirements:
- Cloud publishes with QoS 1
- Vehicle uses `clean_session=false`
- Vehicle subscribes to `c2v/{vehicle_id}/202` before disconnect

## Database Schema (Cloud)

```sql
-- Active jobs per vehicle
CREATE TABLE jobs (
    vehicle_id VARCHAR(64) NOT NULL,
    job_id VARCHAR(64) NOT NULL,

    -- Job definition
    title VARCHAR(256),
    service_name VARCHAR(128),
    method_name VARCHAR(128),
    parameters JSONB DEFAULT '{}'::jsonb,

    -- Schedule
    scheduled_time VARCHAR(64),
    recurrence_rule VARCHAR(128),
    next_run_time VARCHAR(64),

    -- State
    status VARCHAR(32) DEFAULT 'pending',

    -- Wake/sleep policies
    wake_policy SMALLINT DEFAULT 0,
    sleep_policy SMALLINT DEFAULT 0,
    wake_lead_time_s INTEGER DEFAULT 0,

    -- Timestamps
    created_at_ms BIGINT,
    updated_at_ms BIGINT,
    sync_created_at TIMESTAMPTZ DEFAULT NOW(),
    sync_updated_at TIMESTAMPTZ DEFAULT NOW(),

    PRIMARY KEY (vehicle_id, job_id)
);

-- Execution history
CREATE TABLE job_executions (
    id SERIAL PRIMARY KEY,
    vehicle_id VARCHAR(64) NOT NULL,
    job_id VARCHAR(64) NOT NULL,
    status VARCHAR(32),
    executed_at_ms BIGINT,
    duration_ms INTEGER,
    result TEXT,
    error_message TEXT,
    next_run_time VARCHAR(64),
    received_at TIMESTAMPTZ DEFAULT NOW()
);

-- Sync state tracking
CREATE TABLE sync_state (
    vehicle_id VARCHAR(64) PRIMARY KEY,
    scheduler_sequence BIGINT DEFAULT 0,
    scheduler_checksum INTEGER,
    scheduler_last_sync TIMESTAMPTZ
);
```

## Comparison: Discovery vs Scheduler

| Aspect | Discovery | Scheduler |
|--------|-----------|-----------|
| Data nature | Static (schemas) | Dynamic (jobs) |
| Sharing | Fleet-wide (deduplicated) | Per-vehicle (unique) |
| Sync trigger | Startup, service change | Continuous polling |
| Optimization | Hash-first, pull on demand | Delta events, hash-based change detection |
| Direction | Mostly v2c | Bidirectional |
| Commands | Schema request only | Full CRUD operations |
| State size | ~10 unique schemas | ~10-100 jobs per vehicle |

## Current Protocol Characteristics

### Strengths

1. **Efficient change detection**: Hash comparison avoids unnecessary sync
2. **Event batching**: Multiple changes in single message
3. **State checksum**: Enables sync verification
4. **Sequence numbers**: Gap detection for reliability
5. **Idempotent storage**: ON CONFLICT handles duplicates

### Known Limitations

1. **No message-level deduplication**: Relies on DB constraints
2. **Checksum mismatch unhandled**: `request_full_sync` flag not implemented
3. **No command TTL**: Stale commands may execute after long offline period
4. **No command retry**: Cloud doesn't track delivery confirmation
5. **Unbounded terminal job set**: Memory grows without cleanup

## Protocol Improvements

### 1. Command TTL (Time-To-Live)

Commands include expiration to prevent stale execution after long offline periods.

```protobuf
message scheduler_command_t {
    string command_id = 1;
    uint64 timestamp_ns = 2;
    string requester_id = 3;
    command_type_t type = 4;

    uint64 expires_at_ns = 20;      // Command expiration timestamp
    uint32 ttl_seconds = 21;        // Alternative: relative TTL

    oneof payload { ... }
}
```

**Vehicle behavior:**
```cpp
void HandleCommand(const scheduler_command_t& cmd) {
    uint64_t now_ns = GetCurrentTimeNs();

    if (cmd.expires_at_ns() > 0 && now_ns > cmd.expires_at_ns()) {
        // Command expired - send NACK
        SendAck(cmd.command_id(), false, "Command expired");
        return;
    }

    // Execute command...
}
```

**Recommended TTLs:**
| Command Type | Default TTL | Rationale |
|--------------|-------------|-----------|
| CREATE_JOB | 24 hours | Job creation can wait |
| UPDATE_JOB | 1 hour | Updates should be timely |
| DELETE_JOB | 24 hours | Deletion can wait |
| TRIGGER_JOB | 5 minutes | Immediate execution expected |
| PAUSE_JOB | 1 hour | Time-sensitive |
| RESUME_JOB | 1 hour | Time-sensitive |

### 2. Resync Handshake

Cloud can request full resync when checksum mismatch detected.

**New message type:**
```protobuf
message sync_control_t {
    sync_control_type_t type = 1;
    uint32 expected_checksum = 2;   // What cloud has
    uint64 timestamp_ns = 3;
}

enum sync_control_type_t {
    REQUEST_FULL_SYNC = 0;          // Cloud → Vehicle: resync please
    SYNC_VERIFIED = 1;              // Cloud → Vehicle: checksums match
}
```

**Flow:**
```
VEHICLE                                         CLOUD
   │                                              │
   ├──── HEARTBEAT ───────────────────────────────▶
   │     checksum=0xABCD                          │
   │                                              │
   │                      Cloud has checksum=0x1234
   │                      Mismatch detected!      │
   │                                              │
   ◀──── REQUEST_FULL_SYNC ───────────────────────┤
   │     expected_checksum=0x1234                 │
   │                                              │
   ├──── FULL_SYNC ───────────────────────────────▶
   │     [all jobs]                               │
   │     checksum=0xABCD                          │
   │                                              │
   │                      Cloud updates to 0xABCD │
   │                                              │
```

**Vehicle implementation:**
```cpp
void OnSyncControl(const sync_control_t& ctrl) {
    if (ctrl.type() == REQUEST_FULL_SYNC) {
        LOG(INFO) << "Cloud requested full resync";
        TriggerFullSync();
    }
}
```

### 3. Terminal Job Cleanup

Prevent unbounded memory growth from completed jobs.

**Configuration:**
```cpp
struct SyncBridgeConfig {
    // Existing...

    // Terminal job cleanup
    std::chrono::hours terminal_job_retention = 168h;  // 7 days
    std::chrono::minutes cleanup_interval = 60min;     // Check hourly
};
```

**Implementation:**
```cpp
void CleanupTerminalJobs() {
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - config_.terminal_job_retention;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = synced_terminal_jobs_.begin();
         it != synced_terminal_jobs_.end(); ) {
        if (it->second.synced_at < cutoff) {
            it = synced_terminal_jobs_.erase(it);
        } else {
            ++it;
        }
    }
}
```

**Data structure:**
```cpp
struct TerminalJobEntry {
    std::string job_id;
    std::chrono::steady_clock::time_point synced_at;
};

std::unordered_map<std::string, TerminalJobEntry> synced_terminal_jobs_;
```

### 4. Command Delivery Tracking

Cloud tracks command delivery and retries on timeout.

**Database schema:**
```sql
CREATE TABLE pending_commands (
    command_id VARCHAR(64) PRIMARY KEY,
    vehicle_id VARCHAR(64) NOT NULL,
    command_type VARCHAR(32) NOT NULL,
    command_payload BYTEA NOT NULL,

    -- Timing
    created_at TIMESTAMPTZ DEFAULT NOW(),
    expires_at TIMESTAMPTZ NOT NULL,

    -- Delivery tracking
    sent_count INTEGER DEFAULT 1,
    last_sent_at TIMESTAMPTZ DEFAULT NOW(),
    acked_at TIMESTAMPTZ,
    ack_success BOOLEAN,
    ack_error TEXT,

    -- Retry policy
    max_retries INTEGER DEFAULT 3,
    retry_interval_s INTEGER DEFAULT 60
);

CREATE INDEX idx_pending_commands_vehicle ON pending_commands(vehicle_id);
CREATE INDEX idx_pending_commands_expires ON pending_commands(expires_at)
    WHERE acked_at IS NULL;
```

**Cloud flow:**
```
┌─────────────────────────────────────────────────────────────────┐
│ Command Lifecycle                                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  API Request                                                    │
│      │                                                          │
│      ▼                                                          │
│  ┌─────────────────┐                                           │
│  │ INSERT pending_ │                                           │
│  │ commands        │                                           │
│  └────────┬────────┘                                           │
│           │                                                     │
│           ▼                                                     │
│  ┌─────────────────┐     ┌─────────────────┐                   │
│  │ Publish to      │────▶│ MQTT Broker     │                   │
│  │ Kafka/MQTT      │     │ (queues if      │                   │
│  └────────┬────────┘     │  offline)       │                   │
│           │              └─────────────────┘                   │
│           │                                                     │
│           ▼                                                     │
│  ┌─────────────────┐                                           │
│  │ Wait for ACK    │◀──── scheduler_command_ack_t              │
│  │ (timeout: 60s)  │                                           │
│  └────────┬────────┘                                           │
│           │                                                     │
│     ┌─────┴─────┐                                              │
│     │           │                                              │
│     ▼           ▼                                              │
│  ┌──────┐   ┌──────────┐                                       │
│  │ ACK  │   │ Timeout  │                                       │
│  │ recv │   │ /NACK    │                                       │
│  └──┬───┘   └────┬─────┘                                       │
│     │            │                                              │
│     ▼            ▼                                              │
│  ┌──────┐   ┌──────────┐     ┌──────────┐                      │
│  │UPDATE│   │sent_count│────▶│ Retry or │                      │
│  │acked │   │ < max?   │ no  │ Give up  │                      │
│  └──────┘   └────┬─────┘     └──────────┘                      │
│                  │ yes                                          │
│                  ▼                                              │
│             ┌──────────┐                                        │
│             │ Republish│                                        │
│             │ after    │                                        │
│             │ interval │                                        │
│             └──────────┘                                        │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**Retry worker (cloud-side):**
```python
def retry_pending_commands():
    """Background task: retry unacked commands"""
    while True:
        now = datetime.utcnow()

        # Find commands needing retry
        pending = db.query("""
            SELECT * FROM pending_commands
            WHERE acked_at IS NULL
              AND expires_at > NOW()
              AND sent_count < max_retries
              AND last_sent_at < NOW() - (retry_interval_s * INTERVAL '1 second')
        """)

        for cmd in pending:
            # Republish to Kafka
            kafka_producer.produce(
                topic='ifex.scheduler.202',
                key=cmd.vehicle_id,
                value=cmd.command_payload
            )

            db.execute("""
                UPDATE pending_commands
                SET sent_count = sent_count + 1,
                    last_sent_at = NOW()
                WHERE command_id = %s
            """, (cmd.command_id,))

        # Cleanup expired
        db.execute("""
            DELETE FROM pending_commands
            WHERE expires_at < NOW()
              AND acked_at IS NULL
        """)

        time.sleep(10)  # Check every 10 seconds
```

**ACK handler (cloud-side):**
```python
def on_command_ack(ack: scheduler_command_ack_t):
    """Process command acknowledgment from vehicle"""
    db.execute("""
        UPDATE pending_commands
        SET acked_at = NOW(),
            ack_success = %s,
            ack_error = %s
        WHERE command_id = %s
    """, (ack.success, ack.error_message, ack.command_id))
```

### 5. Compression

Apply zstd compression to reduce bandwidth.

**Vehicle-side (before publish):**
```cpp
void PublishSyncMessage(const sync_message_t& msg) {
    std::string serialized = msg.SerializeAsString();

    if (config_.enable_compression && serialized.size() > 256) {
        std::string compressed = ZstdCompress(serialized, config_.compression_level);

        // Prepend compression marker
        std::string payload;
        payload.push_back(0x01);  // Compression flag: zstd
        payload.append(compressed);

        backend_transport_->Publish(CONTENT_ID_SCHEDULER, payload);
    } else {
        std::string payload;
        payload.push_back(0x00);  // Compression flag: none
        payload.append(serialized);

        backend_transport_->Publish(CONTENT_ID_SCHEDULER, payload);
    }
}
```

**Cloud-side (on receive):**
```python
def decode_sync_message(payload: bytes) -> sync_message_t:
    compression_flag = payload[0]
    data = payload[1:]

    if compression_flag == 0x01:
        data = zstd.decompress(data)

    msg = sync_message_t()
    msg.ParseFromString(data)
    return msg
```

**Expected savings:**
| Job Count | Uncompressed | Compressed | Savings |
|-----------|--------------|------------|---------|
| 10 jobs | 2 KB | 800 B | 60% |
| 50 jobs | 10 KB | 3 KB | 70% |
| 100 jobs | 20 KB | 5 KB | 75% |

### 6. Checksum-Only Heartbeat

Reduce heartbeat size when sync is verified.

**Current heartbeat:** Full `sync_message_t` with `active_jobs_count` and `state_checksum`

**Optimized heartbeat:**
```protobuf
message heartbeat_t {
    uint32 active_jobs_count = 1;
    uint32 state_checksum = 2;
    uint64 timestamp_ns = 3;
}
```

**Flow:**
```
VEHICLE                                         CLOUD
   │                                              │
   │  (30s idle, sync verified)                   │
   │                                              │
   ├──── heartbeat_t (12 bytes) ──────────────────▶
   │     count=5, checksum=0xABCD                 │
   │                                              │
   │                      Checksum matches ───────┤
   │                      (no response)           │
   │                                              │
   │  (60s idle, checksum changed)                │
   │                                              │
   ├──── heartbeat_t (12 bytes) ──────────────────▶
   │     count=6, checksum=0xDEF0                 │
   │                                              │
   │                      Mismatch! ──────────────┤
   │                                              │
   ◀──── REQUEST_FULL_SYNC ───────────────────────┤
   │                                              │
```

## Implementation Priority

| Priority | Improvement | Effort | Impact |
|----------|-------------|--------|--------|
| 1 | Command TTL | Low | High - prevents stale execution |
| 2 | Resync handshake | Medium | High - recovers from drift |
| 3 | Terminal job cleanup | Low | Medium - prevents memory leak |
| 4 | Command delivery tracking | Medium | High - reliability |
| 5 | Compression | Low | Medium - bandwidth savings |
| 6 | Checksum-only heartbeat | Low | Low - minor optimization |

## Message Flow Diagram

```
VEHICLE                                         CLOUD
═══════                                         ═════

┌─────────────────┐
│ Scheduler       │
│ Service         │
└────────┬────────┘
         │ gRPC (poll every 1s)
         ▼
┌─────────────────┐
│ SchedulerSync   │
│ Bridge          │
│                 │
│ - Hash jobs     │
│ - Detect deltas │
│ - Batch events  │
└────────┬────────┘
         │ content_id=202
         ▼
┌─────────────────┐                    ┌─────────────────┐
│ Backend         │                    │ mqtt_kafka      │
│ Transport       │ ───v2c/VIN/202───▶ │ _bridge         │
│                 │                    └────────┬────────┘
│                 │                             │
│                 │                             ▼
│                 │                    ┌─────────────────┐
│                 │                    │ Kafka           │
│                 │                    │ ifex.sched.202  │
│                 │                    └────────┬────────┘
│                 │                             │
│                 │                             ▼
│                 │                    ┌─────────────────┐
│                 │                    │ scheduler       │
│                 │                    │ _mirror         │
│                 │                    └────────┬────────┘
│                 │                             │
│                 │                             ▼
│                 │                    ┌─────────────────┐
│                 │                    │ PostgreSQL      │
│                 │                    │ jobs table      │
│                 │ ◀──c2v/VIN/202──── └─────────────────┘
│                 │
│  on_content()   │                    ┌─────────────────┐
│  callback       │                    │ Fleet API       │
└────────┬────────┘                    │ (commands)      │
         │                             └────────┬────────┘
         ▼                                      │
┌─────────────────┐                             │
│ SchedulerSync   │ ◀───────────────────────────┘
│ Bridge          │     scheduler_command_t
│                 │
│ - Decode cmd    │
│ - Execute gRPC  │
│ - Send ACK      │
└─────────────────┘
```

## Appendix: Job Info Structure

```protobuf
message job_info_t {
    string job_id = 1;
    string title = 2;

    // Target service
    string service = 3;
    string method = 4;
    string parameters = 5;          // JSON

    // Schedule
    string scheduled_time = 6;      // ISO8601
    string recurrence_rule = 7;     // iCal RRULE
    string next_run_time = 8;       // Computed

    // State
    job_sync_status_t status = 9;

    // Wake/sleep policies
    uint32 wake_policy = 10;        // 0=NO_WAKE, 1=WAKE_REQUIRED
    uint32 sleep_policy = 11;       // 0=NORMAL, 1=INHIBIT_UNTIL_COMPLETE
    uint32 wake_lead_time_s = 12;

    // Timestamps
    int64 created_at_ms = 13;
    int64 updated_at_ms = 14;
}
```
