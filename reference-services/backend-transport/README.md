# IFEX Backend Transport Service

Bidirectional vehicle-to-cloud communication for IFEX services.

## Overview

The Backend Transport Service provides a common gRPC interface for IFEX services to send and receive data to/from the cloud without knowing the underlying transport protocol (MQTT, SOME/IP, etc.).

```
┌─────────────────────────────────────────────────────────────────────┐
│                        IFEX Services                                 │
│    Climate    Defrost    Telemetry    Diagnostics    ...            │
└───────────────────────────────┬─────────────────────────────────────┘
                                │ gRPC (publish, subscribe)
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│                  Backend Transport Service                           │
│                                                                      │
│   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐          │
│   │ gRPC Server  │───▶│ Message Queue│───▶│ MQTT Client  │──────┐   │
│   │ (100+ clients)│   │ (per content) │   │ (reconnect)  │      │   │
│   └──────────────┘    └──────────────┘    └──────────────┘      │   │
│                                                                  │   │
└──────────────────────────────────────────────────────────────────┼───┘
                                                                   │
                                           MQTT (v2c/*, c2v/*)     │
                                                                   ▼
┌─────────────────────────────────────────────────────────────────────┐
│                           Cloud                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### Key Features

- **100+ concurrent clients** - Single MQTT connection shared across all gRPC clients
- **Per-content-id queues** - Message ordering preserved within each content stream
- **Configurable persistence** - From fire-and-forget to disk-persistent
- **Automatic reconnection** - Exponential backoff, queues messages during disconnect
- **Backpressure signaling** - Clients notified when queues fill up
- **Server-streaming events** - Real-time connection status, delivery acks, incoming content

---

## IFEX Interface Definition

The service is defined using the [COVESA IFEX](https://github.com/COVESA/ifex) standard:

```yaml
name: backend_transport_service
major_version: 1
minor_version: 0
description: >
  Bidirectional backend transport service for vehicle-to-cloud communication.
```

### Data Types

#### Enumerations

| Type | Values | Description |
|------|--------|-------------|
| `connection_state_t` | UNKNOWN, CONNECTED, DISCONNECTED, CONNECTING, RECONNECTING | Transport state |
| `publish_status_t` | OK, BUFFER_FULL, MESSAGE_TOO_LONG, NOT_CONNECTED, TIMEOUT, ERROR | Publish result |
| `persistence_t` | NONE, UNTIL_DELIVERED, UNTIL_RESTART, PERSISTENT | Message durability |

#### Structures

| Type | Fields | Description |
|------|--------|-------------|
| `publish_request_t` | content_id, payload, persistence | Outbound message |
| `publish_response_t` | sequence, status | Assigned sequence for tracking |
| `connection_status_t` | state, reason, timestamp_ns | Connection state |
| `queue_status_t` | is_full, queue_size, queue_capacity | Backpressure info |
| `transport_stats_t` | messages_sent/failed, bytes_sent/received, timestamps | Statistics |
| `content_message_t` | content_id, payload | Incoming c2v message |

### Methods (Request-Response)

| Method | Input | Output | Description |
|--------|-------|--------|-------------|
| `publish` | `publish_request_t` | `publish_response_t` | Queue message for cloud delivery |
| `get_connection_status` | - | `connection_status_t` | Current connection state |
| `get_queue_status` | - | `queue_status_t` | Outbound queue status |
| `get_stats` | - | `transport_stats_t` | Transport statistics |
| `healthy` | - | `boolean` | Health check |

### Events (Server-Streaming)

| Event | Stream Data | Description |
|-------|-------------|-------------|
| `on_content` | `content_message_t` | Incoming cloud messages (c2v) |
| `on_ack` | `delivery_ack_t` | Delivery confirmations with sequence |
| `on_connection_changed` | `connection_status_t` | Connection state changes |
| `on_queue_status_changed` | `queue_status_t` | Backpressure notifications |

---

## API Design Decisions

The IFEX Backend Transport API is designed as a **transport-agnostic abstraction**. This reference implementation uses MQTT directly, but alternative implementations may use different underlying transports (SOME/IP gateways, proprietary protocols, etc.) while exposing the same gRPC interface.

### Persistence Instead of QoS

The API exposes **persistence levels** rather than transport-specific QoS settings:

| Persistence | Semantic Intent | Typical Transport Mapping |
|-------------|-----------------|---------------------------|
| `NONE` | Best-effort, acceptable to lose | QoS 0 / fire-and-forget |
| `UNTIL_DELIVERED` | Must reach broker, retry on failure | QoS 1 / at-least-once |
| `UNTIL_RESTART` | Survive temporary disconnects | QoS 1 + memory queue |
| `PERSISTENT` | Survive power cycles | QoS 1 + disk persistence |

**Rationale:** Clients specify *what they need* (durability guarantees), not *how to achieve it* (protocol-specific QoS). This allows implementations to choose appropriate transport settings.

### Server-Assigned Sequence Numbers

The `publish()` method returns a server-assigned `sequence` number rather than accepting a client-provided message ID.

**Rationale:**
- **Non-blocking design** - The gRPC API is designed to never block. Sequence assignment is atomic and immediate.
- **Monotonic ordering** - Server guarantees strictly increasing sequences per content_id, enabling gap detection.
- **Simpler client logic** - Clients don't need to generate unique IDs or handle ID collisions.

Alternative implementations may internally map sequences to transport-specific IDs as needed.

### Acknowledgments Indicate Success Only

The `on_ack` event stream delivers `delivery_ack_t` messages containing only `content_id` and `sequence`. There is no status field.

**Rationale:**
- **Acks mean success** - An ack is only sent when the message was successfully delivered to the broker.
- **FIFO ordering** - Messages are sent in strict order per content_id. Receiving an ack for sequence N confirms all prior sequences have completed.
- **Gaps indicate failure** - If sequence 5 is acked but sequence 4 was not, message 4 failed.
- **Simpler streaming** - No need to distinguish success/failure variants in the stream.

No timeout tracking is needed. The FIFO guarantee means that once a higher sequence is acked, the fate of all lower sequences is determined.

### Timestamps Without Source Information

The API uses simple `timestamp_ns` (nanoseconds since epoch) rather than structured timestamps with source information (GPS, NTP, system clock, etc.).

**Rationale:**
- **Abstraction level** - Timestamp source is often a transport or envelope concern, not an application concern.
- **Payload responsibility** - Applications requiring precise time correlation should include timestamps in their payload encoding.
- **Implementation flexibility** - Transport implementations may add source metadata at the envelope layer without changing the API.

### Opaque Payloads

The API treats `payload` as opaque bytes. It does not interpret, validate, or transform payload contents.

**Rationale:**
- **Separation of concerns** - The transport layer moves bytes; encoding/decoding is the application's responsibility.
- **Flexibility** - Supports any serialization format (Protobuf, JSON, CBOR, custom binary).
- **No double-encoding** - Avoids nested encoding when payloads are already serialized.

### Content ID Filtering

Event subscriptions (`on_content`, `on_ack`) require a `filter_content_ids[]` parameter specifying which content IDs to receive.

**Rationale:**
- **Explicit subscription** - Clients only receive events they're interested in, reducing noise.
- **Scalability** - With many content streams, filtering at the server reduces client-side processing.
- **Security boundary** - Implementations can enforce access control per content_id.

---

## Architecture

### Components

```
┌─────────────────────────────────────────────────────────────────────┐
│                    BackendTransportServer                            │
│                                                                      │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │                      gRPC Services                             │  │
│  │  publish_service, get_connection_status_service, ...          │  │
│  │  on_content_service, on_ack_service, on_connection_changed_.. │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                              │                                       │
│              ┌───────────────┼───────────────┐                      │
│              ▼               ▼               ▼                      │
│  ┌────────────────┐  ┌────────────────┐  ┌────────────────┐         │
│  │ Stream Manager │  │ MessageQueue   │  │  Statistics    │         │
│  │ (subscribers)  │  │   Manager      │  │  (atomics)     │         │
│  └────────────────┘  └───────┬────────┘  └────────────────┘         │
│                              │                                       │
│                              ▼                                       │
│                      ┌────────────────┐                              │
│                      │  MqttClient    │                              │
│                      │  (mosquitto)   │                              │
│                      └────────────────┘                              │
└─────────────────────────────────────────────────────────────────────┘
```

### Message Queue Manager

Per-content-id queues ensure message ordering:

```
Client A (content_id=1) ─┐
                         │    ┌─────────────────────┐
Client B (content_id=1) ─┼───▶│ Queue content_id=1  │──┐
                         │    │ [msg1, msg2, msg3]  │  │
Client C (content_id=1) ─┘    └─────────────────────┘  │
                                                       │
Client D (content_id=2) ─────▶│ Queue content_id=2  │──┼──▶ Sender Thread
                              │ [msg1, msg2]        │  │    (round-robin)
                              └─────────────────────┘  │
                                                       │
Client E (content_id=3) ─────▶│ Queue content_id=3  │──┘
                              │ [msg1]              │
                              └─────────────────────┘
```

**Design decisions:**
- One queue per content_id (created on demand)
- Single sender thread for fairness across queues
- Round-robin dequeue prevents starvation
- Messages sent only when MQTT is connected

### MQTT Client

Resilient MQTT connectivity with automatic reconnection:

```
                    ┌─────────────────┐
     Connect() ────▶│  Loop Thread    │◀──── Callbacks
                    │                 │
                    │  if disconnected:
                    │    exponential backoff
                    │    retry connect
                    │                 │
                    │  if connected:  │
                    │    process msgs │
                    │    handle acks  │
                    └─────────────────┘
```

**Reconnection behavior:**
- Initial connection failure does NOT prevent service startup
- Loop thread continuously retries with exponential backoff (1s → 30s)
- Messages queue during disconnect, delivered on reconnect
- Connection status broadcast to all subscribers

### Persistence Levels

| Level | Behavior |
|-------|----------|
| `NONE` | Fire-and-forget. Dropped if queue full. |
| `UNTIL_DELIVERED` | Retried until MQTT ack. Lost on restart. |
| `UNTIL_RESTART` | Kept in memory. Persisted on graceful shutdown. |
| `PERSISTENT` | Written to disk immediately. Survives power cycles. |

When queue is full:
- `NONE` messages are rejected immediately
- `UNTIL_DELIVERED`+ messages can displace `NONE` messages

---

## Configuration

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `MQTT_HOST` | localhost | MQTT broker hostname |
| `MQTT_PORT` | 1883 | MQTT broker port |
| `MQTT_USERNAME` | - | MQTT authentication |
| `MQTT_PASSWORD` | - | MQTT authentication |
| `VEHICLE_ID` | vehicle-001 | Vehicle identifier for topics |
| `QUEUE_SIZE` | 1000 | Queue size per content_id |
| `PERSISTENCE_DIR` | /var/lib/ifex/backend-transport | Message persistence directory |
| `SERVICE_DISCOVERY_ENDPOINT` | - | Discovery service address |

### Command Line

```bash
ifex-backend-transport-service \
  --port=50060 \
  --mqtt-host=broker.example.com \
  --mqtt-port=8883 \
  --vehicle-id=VIN123456789 \
  --queue-size=500
```

### MQTT Topics

| Direction | Pattern | Example |
|-----------|---------|---------|
| Vehicle → Cloud | `v2c/{vehicle_id}/{content_id}` | `v2c/vehicle-001/42` |
| Cloud → Vehicle | `c2v/{vehicle_id}/{content_id}` | `c2v/vehicle-001/100` |

---

## Client Library

A C++ client library simplifies integration:

### Basic Usage

```cpp
#include "backend_transport_client.hpp"

using namespace ifex::client;

// Create client for a specific content_id
auto channel = grpc::CreateChannel("localhost:50060",
                                   grpc::InsecureChannelCredentials());
BackendTransportClient client(channel, 42);  // content_id=42

// Publish data
std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
auto result = client.publish(payload, Persistence::UntilDelivered);

if (result.ok()) {
    std::cout << "Sent with sequence " << result.sequence << "\n";
}
```

### Streaming Subscriptions

```cpp
// Monitor connection status
client.on_connection_changed([](const ConnectionStatus& status) {
    if (status.state == ConnectionState::Connected) {
        std::cout << "Connected to cloud\n";
    } else {
        std::cout << "Disconnected: " << status.reason << "\n";
    }
});

// Receive delivery confirmations
client.on_ack([](uint64_t sequence) {
    std::cout << "Message " << sequence << " delivered\n";
});

// Receive incoming cloud messages
client.on_content([](const std::vector<uint8_t>& payload) {
    process_cloud_message(payload);
});

// Cleanup
client.unsubscribe_all();
```

### Status Queries

```cpp
// Health check
bool healthy = client.healthy();

// Connection details
auto conn = client.connection_status();
std::cout << "State: " << static_cast<int>(conn.state) << "\n";

// Queue status (backpressure)
auto queue = client.queue_status();
if (queue.is_full) {
    std::cout << "Queue full! Slow down.\n";
}

// Statistics
auto stats = client.stats();
std::cout << "Sent: " << stats.messages_sent << " messages\n";
```

---

## Integration Patterns

### With IFEX Services

Services use Backend Transport for cloud telemetry:

```cpp
class TelemetryService {
public:
    TelemetryService(std::shared_ptr<grpc::Channel> transport_channel)
        : transport_(transport_channel, TELEMETRY_CONTENT_ID) {}

    void report_metrics(const Metrics& m) {
        auto payload = serialize(m);
        auto result = transport_.publish(payload, Persistence::UntilDelivered);

        if (!result.ok()) {
            LOG(WARNING) << "Failed to send telemetry";
        }
    }

private:
    BackendTransportClient transport_;
    static constexpr uint32_t TELEMETRY_CONTENT_ID = 1;
};
```

### With Dispatcher

Services can call Backend Transport via Dispatcher (JSON):

```bash
# Publish via Dispatcher (no protobuf knowledge needed)
grpcurl -d '{
  "call": {
    "service_name": "backend-transport",
    "method_name": "publish",
    "parameters": "{\"content_id\": 42, \"payload\": \"SGVsbG8=\"}"
  }
}' localhost:50052 swdv.ifex_dispatcher.call_method_service/call_method
```

### Multiple Content IDs

Different data streams use different content_ids:

```cpp
// Telemetry: content_id=1
BackendTransportClient telemetry(channel, 1);

// Diagnostics: content_id=2
BackendTransportClient diagnostics(channel, 2);

// Events: content_id=3
BackendTransportClient events(channel, 3);

// Each has independent:
// - Sequence numbering
// - Queue with ordering guarantees
// - MQTT topic (v2c/{vehicle}/1, v2c/{vehicle}/2, ...)
```

---

## Testing

### Unit Tests

Located in `tests/`:

```bash
# Run all tests
ctest --test-dir build -R "backend_transport" --output-on-failure

# Integration tests (require Docker for MQTT broker)
./build/reference-services/backend-transport/ifex-backend-transport-integration-test

# Resilience tests (broker up/down scenarios)
./build/reference-services/backend-transport/ifex-backend-transport-resilience-test
```

### Test Categories

| Category | Tests | Requirements |
|----------|-------|--------------|
| Integration | Publish, sequences, status, stats | Docker (MQTT broker) |
| Resilience | Reconnection, queueing, persistence | Docker (MQTT broker) |

### Test Fixture

The `MqttTestFixture` automatically manages Docker containers:

```cpp
class MyTest : public MqttTestFixture {
protected:
    void SetUp() override {
        MqttTestFixture::SetUp();
        // MQTT broker running on port 11883
    }
};
```

### External MQTT Broker

Skip Docker by setting environment variables:

```bash
MQTT_HOST=192.168.1.100 MQTT_PORT=1883 \
  ./ifex-backend-transport-integration-test
```

---

## Deployment

### As Standalone Service

```bash
# Start with defaults
./ifex-backend-transport-service

# With configuration
./ifex-backend-transport-service \
  --mqtt-host=broker.internal \
  --vehicle-id=$(cat /etc/vehicle-id)
```

### With Docker

```dockerfile
FROM tvw-runtime:ubi

ENV MQTT_HOST=mosquitto
ENV VEHICLE_ID=vehicle-001

CMD ["ifex-backend-transport-service"]
```

### Health Monitoring

```bash
# gRPC health check
grpcurl -plaintext localhost:50060 \
  swdv.backend_transport_service.healthy_service/healthy

# Returns: { "is_healthy": true }
```

---

## Files

```
backend-transport/
├── include/
│   ├── backend_transport_server.hpp   # Server interface
│   ├── mqtt_client.hpp                # MQTT wrapper
│   └── message_queue.hpp              # Queue manager
├── src/
│   ├── backend_transport_server.cpp   # gRPC implementation
│   ├── mqtt_client.cpp                # Mosquitto wrapper
│   ├── message_queue.cpp              # Per-content queues
│   └── main.cpp                       # Service entry point
├── client/
│   ├── include/backend_transport_client.hpp  # Client API
│   └── src/backend_transport_client.cpp
├── tests/
│   ├── backend_transport_integration_test.cpp
│   ├── backend_transport_resilience_test.cpp
│   └── mqtt_test_fixture.hpp
└── CMakeLists.txt
```

---

## See Also

- [IFEX Service Architecture](../../docs/ifex-service-architecture.md) - How to build IFEX services
- [Core Services Specification](../../docs/core-services-spec.md) - Infrastructure services
- [COVESA IFEX](https://github.com/COVESA/ifex) - Interface Exchange standard
