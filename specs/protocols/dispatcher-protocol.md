# Dispatcher Protocol Specification

## Overview

The Dispatcher Protocol enables synchronous method invocation from cloud to vehicle. Unlike Discovery (static schemas) and Scheduler (async jobs), the Dispatcher Protocol provides request-response semantics with timeout guarantees.

This protocol defines how the Cloud Dispatcher Service communicates with the vehicle-side Dispatcher Bridge to invoke methods on vehicle services.

## Design Principles

1. **Correlation-based**: Each request has unique ID for response matching
2. **Timeout-aware**: All requests have explicit timeout
3. **Idempotent handling**: Duplicate requests detected and rejected
4. **Ordered per-vehicle**: MQTT QoS 1 ensures delivery order

## Content ID

All Dispatcher messages use **content_id=200**.

| Direction | Topic Pattern | Payload |
|-----------|---------------|---------|
| c2v | `c2v/{vehicle_id}/200` | `dispatcher_request_t` |
| v2c | `v2c/{vehicle_id}/200` | `dispatcher_response_t` |

## Message Types

> **Note:** The wire format uses `dispatcher_request_t` and `dispatcher_response_t` message names.
> Legacy implementations may use `rpc_request_t` / `rpc_response_t` names.

### Request (Cloud → Vehicle)

```protobuf
message rpc_request_t {
    string correlation_id = 1;        // Unique request ID (UUID)
    string service_name = 2;          // Target service
    string method_name = 3;           // Target method
    string parameters_json = 4;       // Method parameters as JSON
    uint32 timeout_ms = 5;            // Request timeout
    sint64 request_timestamp_ms = 6;  // Cloud timestamp (epoch ms)

    // Improvements (new fields)
    uint64 expires_at_ms = 10;        // Absolute expiration (epoch ms)
    string idempotency_key = 11;      // For exactly-once semantics
    rpc_priority_t priority = 12;     // Execution priority
    string trace_id = 13;             // Distributed tracing
}

enum rpc_priority_t {
    LOW = 0;
    NORMAL = 1;
    HIGH = 2;
    CRITICAL = 3;
}
```

### Response (Vehicle → Cloud)

```protobuf
message rpc_response_t {
    string correlation_id = 1;        // Matches request
    rpc_status_t status = 2;          // Execution result
    string result_json = 3;           // Return value as JSON
    string error_message = 4;         // Error details if failed
    uint32 duration_ms = 5;           // Actual execution time
    string service_endpoint = 6;      // Service address used
    sint64 response_timestamp_ms = 7; // Vehicle timestamp (epoch ms)
}

enum rpc_status_t {
    SUCCESS = 0;
    FAILED = 1;
    TIMEOUT = 2;
    SERVICE_UNAVAILABLE = 3;
    METHOD_NOT_FOUND = 4;
    INVALID_PARAMETERS = 5;
    TRANSPORT_ERROR = 6;
    DUPLICATE_REQUEST = 7;
    EXPIRED = 8;                      // New: request expired before execution
}
```

## Protocol Flow

### Basic Request-Response

```
CLOUD                                           VEHICLE
   │                                              │
   ├──── rpc_request_t ───────────────────────────▶
   │     correlation_id=abc-123                   │
   │     service=climate_comfort                  │
   │     method=set_comfort                       │
   │     parameters={"mode": 1}                   │
   │     timeout_ms=5000                          │
   │                                              │
   │                          DispatcherBridge    │
   │                          validates, queues   │
   │                                              │
   │                          Dispatcher.call()   │
   │                                              │
   ◀──── rpc_response_t ──────────────────────────┤
   │     correlation_id=abc-123                   │
   │     status=SUCCESS                           │
   │     result={"accepted": true}                │
   │     duration_ms=127                          │
   │                                              │
```

### Timeout Scenario

```
CLOUD                                           VEHICLE
   │                                              │
   ├──── rpc_request_t ───────────────────────────▶
   │     timeout_ms=1000                          │
   │                                              │
   │                          Service takes 2s... │
   │                                              │
   │     (1s timeout checker)                     │
   │                                              │
   ◀──── rpc_response_t ──────────────────────────┤
   │     status=TIMEOUT                           │
   │     error="Request timed out"                │
   │                                              │
```

### Offline Vehicle

```
CLOUD                                           VEHICLE (offline)
   │                                              │
   ├──── rpc_request_t ───────────────────────────▶
   │     expires_at_ns=T+1hour                   ╳│ (queued at broker)
   │                                              │
   │                                              │
   │                                      (reconnects after 30min)
   │                                              │
   │                          Check: now < expires│
   │                          Execute request     │
   │                                              │
   ◀──── rpc_response_t ──────────────────────────┤
   │                                              │
```

## Vehicle-Side Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│ Backend Transport Service (port 50060)                          │
│                                                                 │
│  on_content(content_id=200, payload)                           │
│      │                                                          │
│      ▼                                                          │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │ DispatcherBridge (port 50059)                             │  │
│  │                                                           │  │
│  │  Validation:                                              │  │
│  │  ├─ correlation_id not empty                              │  │
│  │  ├─ Not duplicate (check pending_requests_)               │  │
│  │  ├─ Under concurrent limit (100)                          │  │
│  │  ├─ Not expired (expires_at_ns > now)                     │  │
│  │  └─ Remaining timeout > 0                                 │  │
│  │                                                           │  │
│  │  Worker Pool (4 threads):                                 │  │
│  │  ├─ Queue request                                         │  │
│  │  ├─ Call Dispatcher.call_method() with remaining timeout  │  │
│  │  └─ Send response via Backend Transport                   │  │
│  │                                                           │  │
│  │  Timeout Checker (1s interval):                           │  │
│  │  ├─ Scan pending_requests_                                │  │
│  │  └─ Send TIMEOUT response for overdue                     │  │
│  └──────────────────────────────────────────────────────────┘  │
│      │                                                          │
│      ▼                                                          │
│  Dispatcher Service (port 50052)                               │
│      │                                                          │
│      ▼                                                          │
│  Target Service (dynamic lookup)                               │
└─────────────────────────────────────────────────────────────────┘
```

## Cloud-Side Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│ Fleet API                                                       │
│                                                                 │
│  POST /api/vehicles/{vid}/rpc                                  │
│      │                                                          │
│      ├─ Generate correlation_id (UUID)                         │
│      ├─ Set expires_at_ns = now + timeout_ms                   │
│      ├─ INSERT into pending_rpc_requests                       │
│      │                                                          │
│      ▼                                                          │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │ Kafka Producer                                            │  │
│  │  topic: ifex.rpc.200                                      │  │
│  │  key: vehicle_id                                          │  │
│  └──────────────────────────────────────────────────────────┘  │
│      │                                                          │
│      ▼                                                          │
│  mqtt_kafka_bridge → MQTT (c2v/{vid}/200)                      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ RPC Gateway (response handler)                                  │
│                                                                 │
│  Kafka Consumer (ifex.rpc.200)                                 │
│      │                                                          │
│      ├─ Decode rpc_response_t                                  │
│      ├─ UPDATE rpc_requests SET responded_at, status, result   │
│      │                                                          │
│  Timeout Checker (30s interval):                               │
│      ├─ Mark unresponded requests as TIMEOUT                   │
│      └─ WHERE responded_at IS NULL AND now > created_at + timeout
└─────────────────────────────────────────────────────────────────┘
```

## Database Schema

```sql
CREATE TABLE rpc_requests (
    id SERIAL PRIMARY KEY,
    correlation_id VARCHAR(64) UNIQUE NOT NULL,
    vehicle_id VARCHAR(64) NOT NULL,

    -- Request
    service_name VARCHAR(128),
    method_name VARCHAR(128),
    parameters_json TEXT,
    timeout_ms INTEGER,
    priority SMALLINT DEFAULT 1,
    idempotency_key VARCHAR(64),
    trace_id VARCHAR(64),

    -- Timing
    request_timestamp_ms BIGINT,
    expires_at_ms BIGINT,
    created_at TIMESTAMPTZ DEFAULT NOW(),

    -- Response
    response_status VARCHAR(32),
    result_json TEXT,
    error_message TEXT,
    duration_ms INTEGER,
    responded_at TIMESTAMPTZ,

    -- Retry tracking
    sent_count INTEGER DEFAULT 1,
    last_sent_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_rpc_correlation ON rpc_requests(correlation_id);
CREATE INDEX idx_rpc_vehicle ON rpc_requests(vehicle_id);
CREATE INDEX idx_rpc_pending ON rpc_requests(responded_at)
    WHERE responded_at IS NULL;
CREATE INDEX idx_rpc_idempotency ON rpc_requests(idempotency_key)
    WHERE idempotency_key IS NOT NULL;
```

## Protocol Improvements

### 1. Expiration Check (Fix Clock Skew)

**Problem:** Using cloud timestamp with vehicle clock causes premature expiration.

**Solution:** Use absolute expiration, not clock arithmetic.

```cpp
// Cloud sets absolute expiration
request.set_expires_at_ms(now_ms + timeout_ms);

// Vehicle checks against own clock
void HandleRequest(const rpc_request_t& req) {
    if (req.expires_at_ms() > 0 && NowMs() > req.expires_at_ms()) {
        SendResponse(req.correlation_id(), EXPIRED,
                    "", "Request expired", 0);
        return;
    }
    // Process request...
}
```

**Tolerance:** Allow 5-minute clock skew before rejecting.

### 2. Request TTL by Type

Different operations have different urgency:

| Operation Type | Default TTL | Rationale |
|---------------|-------------|-----------|
| Read state | 30 seconds | Quick query |
| Set preference | 5 minutes | User can wait |
| Lock/unlock | 30 seconds | Security-sensitive |
| Emergency | 10 seconds | Critical |

```cpp
uint32_t GetDefaultTTL(const std::string& method) {
    if (method.find("emergency") != npos) return 10000;
    if (method.find("lock") != npos) return 30000;
    if (method.find("get_") == 0) return 30000;
    return 300000;  // 5 minutes default
}
```

### 3. Delivery Confirmation

Track whether vehicle received and acknowledged request.

**New message type:**
```protobuf
message rpc_ack_t {
    string correlation_id = 1;
    rpc_ack_type_t type = 2;
    uint64 timestamp_ms = 3;
}

enum rpc_ack_type_t {
    RECEIVED = 0;     // Vehicle received request
    PROCESSING = 1;   // Started execution
    COMPLETED = 2;    // Execution finished (redundant with response)
}
```

**Flow:**
```
CLOUD                                           VEHICLE
   │                                              │
   ├──── rpc_request_t ───────────────────────────▶
   │                                              │
   ◀──── rpc_ack_t (RECEIVED) ────────────────────┤
   │                                              │
   │                          (processing...)     │
   │                                              │
   ◀──── rpc_response_t ──────────────────────────┤
   │                                              │
```

**Cloud retry logic:**
```python
def wait_for_response(correlation_id, timeout_ms):
    start = now()

    # Wait for RECEIVED ack (short timeout)
    ack = wait_for_ack(correlation_id, timeout=5000)
    if not ack:
        # Retry send
        resend_request(correlation_id)
        return wait_for_response(correlation_id, timeout_ms - 5000)

    # Wait for response (remaining timeout)
    remaining = timeout_ms - (now() - start)
    return wait_for_response_payload(correlation_id, remaining)
```

### 4. Retry with Exponential Backoff

Cloud retries unacknowledged requests:

```python
def send_with_retry(request, max_retries=3):
    for attempt in range(max_retries):
        send_request(request)

        # Wait for ACK with exponential backoff
        wait_time = min(1000 * (2 ** attempt), 10000)  # 1s, 2s, 4s, max 10s

        ack = wait_for_ack(request.correlation_id, wait_time)
        if ack:
            return wait_for_response(request.correlation_id)

        # Log retry
        log.warning(f"Retry {attempt+1}/{max_retries} for {request.correlation_id}")

    raise RPCDeliveryError("Max retries exceeded")
```

### 5. Priority Queue

Vehicle processes high-priority requests first:

```cpp
struct PendingRequest {
    rpc_priority_t priority;
    std::chrono::time_point deadline;
    rpc_request_t request;
};

// Priority queue: higher priority first, then earlier deadline
struct RequestComparator {
    bool operator()(const PendingRequest& a, const PendingRequest& b) {
        if (a.priority != b.priority)
            return a.priority < b.priority;  // Higher priority first
        return a.deadline > b.deadline;      // Earlier deadline first
    }
};

std::priority_queue<PendingRequest, vector<PendingRequest>,
                    RequestComparator> request_queue_;
```

### 6. Idempotency Key

For non-idempotent operations, ensure exactly-once execution:

```cpp
// Vehicle-side cache
std::unordered_map<std::string, rpc_response_t> idempotency_cache_;
std::chrono::hours cache_ttl_ = 24h;

void HandleRequest(const rpc_request_t& req) {
    if (!req.idempotency_key().empty()) {
        auto it = idempotency_cache_.find(req.idempotency_key());
        if (it != idempotency_cache_.end()) {
            // Return cached response
            SendResponse(it->second);
            return;
        }
    }

    // Execute and cache
    auto response = ExecuteRequest(req);

    if (!req.idempotency_key().empty()) {
        idempotency_cache_[req.idempotency_key()] = response;
    }

    SendResponse(response);
}
```

### 7. Circuit Breaker

Fail fast when service is consistently failing:

```cpp
class CircuitBreaker {
    enum State { CLOSED, OPEN, HALF_OPEN };

    State state_ = CLOSED;
    int failure_count_ = 0;
    int success_count_ = 0;
    std::chrono::time_point last_failure_;

    static constexpr int FAILURE_THRESHOLD = 5;
    static constexpr auto RECOVERY_TIME = 30s;

public:
    bool AllowRequest() {
        if (state_ == CLOSED) return true;

        if (state_ == OPEN) {
            if (now() - last_failure_ > RECOVERY_TIME) {
                state_ = HALF_OPEN;
                return true;
            }
            return false;
        }

        // HALF_OPEN: allow one request
        return true;
    }

    void RecordSuccess() {
        if (state_ == HALF_OPEN) {
            state_ = CLOSED;
            failure_count_ = 0;
        }
        success_count_++;
    }

    void RecordFailure() {
        failure_count_++;
        last_failure_ = now();

        if (failure_count_ >= FAILURE_THRESHOLD) {
            state_ = OPEN;
        }
    }
};

std::unordered_map<std::string, CircuitBreaker> circuit_breakers_;  // Per service
```

### 8. Distributed Tracing

Propagate trace context for observability:

```protobuf
message rpc_request_t {
    // ... existing fields ...

    string trace_id = 13;           // W3C Trace Context
    string span_id = 14;
    string trace_flags = 15;
}
```

```cpp
// Vehicle creates child span
void ExecuteRequest(const rpc_request_t& req) {
    auto span = tracer_->StartSpan(
        "dispatcher.call_method",
        {{"parent_trace_id", req.trace_id()},
         {"parent_span_id", req.span_id()}}
    );

    // ... execute ...

    span->End();
}
```

## Implementation Priority

| Priority | Improvement | Effort | Impact |
|----------|-------------|--------|--------|
| 1 | Expiration check fix | Low | Critical - fixes clock skew |
| 2 | Delivery confirmation | Medium | High - reliability |
| 3 | Retry with backoff | Medium | High - reliability |
| 4 | Request TTL by type | Low | Medium - prevents stale execution |
| 5 | Priority queue | Low | Medium - responsiveness |
| 6 | Idempotency key | Medium | Medium - exactly-once |
| 7 | Circuit breaker | Medium | Medium - resilience |
| 8 | Distributed tracing | Low | Low - observability |

## Comparison: Dispatcher vs Scheduler

| Aspect | Dispatcher (200) | Scheduler (202) |
|--------|------------------|-----------------|
| Semantics | Synchronous | Asynchronous |
| Timeout | Per-request | Per-job (execution) |
| Response | Required | Optional (ACK) |
| Retry | Cloud-managed | Vehicle-managed |
| State | Stateless | Stateful (job lifecycle) |
| Use case | Immediate action | Deferred/recurring action |

## Error Handling Matrix

| Scenario | Status Code | Cloud Action | Vehicle Action |
|----------|-------------|--------------|----------------|
| Service not found | SERVICE_UNAVAILABLE | Log, notify user | N/A |
| Method not found | METHOD_NOT_FOUND | Log, notify user | N/A |
| Invalid params | INVALID_PARAMETERS | Log, notify user | N/A |
| Timeout | TIMEOUT | Retry or notify | Send timeout response |
| Expired before exec | EXPIRED | Log as stale | Skip execution |
| Duplicate ID | DUPLICATE_REQUEST | Ignore | Return cached/reject |
| Concurrent limit | TRANSPORT_ERROR | Backoff, retry | Reject with backpressure |
| Dispatcher crash | TRANSPORT_ERROR | Retry | N/A |
