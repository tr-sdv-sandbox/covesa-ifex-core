# IFEX Cloud Backend Transport Service

Cloud-side counterpart to the vehicle Backend Transport Service for bidirectional vehicle-to-cloud communication.

## Overview

The Cloud Backend Transport Service provides a gRPC interface for cloud services to send messages to vehicles and receive messages from them. It connects directly to MQTT and pairs with the vehicle-side [Backend Transport Service](../../reference-services/backend-transport/README.md).

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Cloud Services                                │
│    Dispatcher    Scheduler    Analytics    Diagnostics    ...        │
└───────────────────────────────┬─────────────────────────────────────┘
                                │ gRPC (send, subscribe)
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│               Cloud Backend Transport Service                        │
│                                                                      │
│   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐          │
│   │ gRPC Server  │    │ Vehicle State │   │ MQTT Client  │──────┐   │
│   │ (streams)    │◀──▶│ (per-vehicle) │◀──│ (reconnect)  │      │   │
│   └──────────────┘    └──────────────┘    └──────────────┘      │   │
│                                                                  │   │
└──────────────────────────────────────────────────────────────────┼───┘
                                                                   │
                                      MQTT (v2c/*, c2v/*, is_online)
                                                                   │
┌──────────────────────────────────────────────────────────────────┼───┐
│                           MQTT Broker                             │   │
└──────────────────────────────────────────────────────────────────┼───┘
                                                                   │
                                                                   ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    Vehicle Backend Transport                         │
│                    (reference-services/backend-transport)            │
└─────────────────────────────────────────────────────────────────────┘
```

### Key Features

- **Direct MQTT connection** - Simple reference implementation, no Kafka required
- **Per-vehicle state tracking** - Online/offline status, sequence numbers
- **Bidirectional messaging** - Send to vehicles (c2v) and receive from vehicles (v2c)
- **Server-streaming events** - Real-time message delivery, status changes, ACKs
- **Partitioning support** - Optional content_id-based partitioning for horizontal scaling

### Production vs. Reference

This is a **reference implementation** for testing and development:
- Connects directly to MQTT (no Kafka)
- Single partition (handles all vehicles)
- No persistence or high availability

For production deployments, use the Kafka-based implementation in `covesa-ifex-offboard-services` which provides:
- Kafka consumer groups for horizontal scaling
- PostgreSQL persistence
- Multi-partition support

---

## IFEX Interface Definition

Defined in [`ifex/cloud-backend-transport-service.yml`](../ifex/cloud-backend-transport-service.yml):

### Data Types

#### Enumerations

| Type | Values | Description |
|------|--------|-------------|
| `vehicle_status_t` | UNKNOWN, ONLINE, OFFLINE | Vehicle connection state |
| `persistence_t` | BEST_EFFORT, VOLATILE, DURABLE | Delivery guarantee level |
| `publish_status_t` | OK, QUEUE_FULL, VEHICLE_OFFLINE, INVALID_REQUEST | Send result |
| `queue_level_t` | EMPTY, LOW, NORMAL, HIGH, CRITICAL, FULL | Queue fill level |

#### Structures

| Type | Fields | Description |
|------|--------|-------------|
| `send_request_t` | vehicle_id, payload, persistence | Outbound c2v message |
| `send_response_t` | sequence, status, queue_level | Send result with sequence |
| `vehicle_message_t` | vehicle_id, payload, sequence, timestamp_ms | Incoming v2c message |
| `vehicle_status_event_t` | vehicle_id, status, timestamp_ms | Status change event |
| `delivery_ack_t` | vehicle_id, sequence | Delivery confirmation |
| `channel_info_t` | content_id, partition_id, total_partitions | Channel binding |
| `queue_status_t` | level, queue_size, queue_capacity | Queue state |
| `transport_stats_t` | messages_sent/received, bytes_sent/received, ... | Statistics |

### Methods (Request-Response)

| Method | Input | Output | Description |
|--------|-------|--------|-------------|
| `send_to_vehicle` | `send_request_t` | `send_response_t` | Send message to a vehicle |
| `get_vehicle_status` | vehicle_id | status, last_seen_ms | Query vehicle state |
| `get_channel_info` | - | `channel_info_t` | Get partition binding |
| `get_queue_status` | vehicle_id | `queue_status_t` | Query outbound queue |
| `get_stats` | - | `transport_stats_t` | Get transport statistics |
| `healthy` | - | boolean | Health check |

### Events (Server-Streaming)

| Event | Stream Data | Description |
|-------|-------------|-------------|
| `on_vehicle_message` | `vehicle_message_t` | Incoming vehicle messages (v2c) |
| `on_ack` | `delivery_ack_t` | Delivery confirmations |
| `on_vehicle_status` | `vehicle_status_event_t` | Vehicle online/offline changes |
| `on_queue_status_changed` | `queue_status_t` | Backpressure notifications |

---

## Architecture

### Vehicle Online/Offline Detection

The service tracks vehicle connection status via MQTT retained messages:

```
Vehicle connects:
  1. Sets LWT (Last Will and Testament): v2c/{vehicle_id}/is_online = "0"
  2. Publishes: v2c/{vehicle_id}/is_online = "1" (retained)

Vehicle graceful disconnect:
  1. Publishes: v2c/{vehicle_id}/is_online = "0" (retained)
  2. Disconnects cleanly

Vehicle crash (ungraceful):
  1. Broker publishes LWT: v2c/{vehicle_id}/is_online = "0" (retained)
```

The cloud service subscribes to `v2c/+/is_online` and maintains per-vehicle state:

| Event | MQTT Message | Cloud State |
|-------|--------------|-------------|
| Vehicle connects | `is_online = "1"` | ONLINE |
| Vehicle graceful stop | `is_online = "0"` | OFFLINE |
| Vehicle crashes | LWT `is_online = "0"` | OFFLINE |
| No status received | - | UNKNOWN |

### Message Flow

**Cloud → Vehicle (c2v):**
```
CloudService.SendToVehicle(vehicle_id, payload)
       │
       ▼
CloudBackendTransportServer
       │ MQTT publish: c2v/{vehicle_id}/{content_id}
       ▼
    MQTT Broker
       │
       ▼
BackendTransportServer (vehicle)
       │ on_content event
       ▼
VehicleService receives payload
```

**Vehicle → Cloud (v2c):**
```
VehicleService.Publish(payload)
       │
       ▼
BackendTransportServer (vehicle)
       │ MQTT publish: v2c/{vehicle_id}/{content_id}
       ▼
    MQTT Broker
       │
       ▼
CloudBackendTransportServer
       │ on_vehicle_message event
       ▼
CloudService receives payload
```

### Partitioning Model

For horizontal scaling, multiple cloud transport instances can partition vehicles:

```
CloudBackendTransport (partition 0/3) → handles vehicles A, D, G, ...
CloudBackendTransport (partition 1/3) → handles vehicles B, E, H, ...
CloudBackendTransport (partition 2/3) → handles vehicles C, F, I, ...
```

Partition assignment: `hash(vehicle_id) % total_partitions == partition_id`

For single-instance testing: `partition_id=0, total_partitions=1` (handles all vehicles).

---

## Configuration

### Server Config

```cpp
CloudBackendTransportServer::Config config;
config.mqtt_host = "localhost";           // MQTT broker hostname
config.mqtt_port = 1883;                  // MQTT broker port
config.mqtt_username = "";                // Optional authentication
config.mqtt_password = "";                // Optional authentication
config.content_id = 200;                  // Content ID to handle
config.partition_id = 0;                  // This instance's partition
config.total_partitions = 1;              // Total partitions (1 = all vehicles)
config.v2c_prefix = "v2c";                // Vehicle-to-cloud topic prefix
config.c2v_prefix = "c2v";                // Cloud-to-vehicle topic prefix
```

### Command Line (Standalone Service)

```bash
./ifex-cloud-backend-transport-service \
  --listen=0.0.0.0:50100 \
  --mqtt_host=broker.example.com \
  --mqtt_port=1883 \
  --content_id=200 \
  --partition_id=0 \
  --total_partitions=1
```

### MQTT Topics

| Direction | Pattern | Example |
|-----------|---------|---------|
| Vehicle → Cloud | `v2c/{vehicle_id}/{content_id}` | `v2c/VIN123/200` |
| Cloud → Vehicle | `c2v/{vehicle_id}/{content_id}` | `c2v/VIN123/200` |
| Vehicle Status | `v2c/{vehicle_id}/is_online` | `v2c/VIN123/is_online` |

---

## Client Library

### Basic Usage

```cpp
#include "cloud_backend_transport_client.hpp"

using namespace ifex::cloud;
using namespace swdv::cloud_backend_transport_service;

// Connect to cloud transport service
CloudBackendTransportClient client("localhost:50100");

// Send message to a vehicle
std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
auto result = client.SendToVehicle("VIN123", payload);

if (result.status() == publish_status_t::OK) {
    std::cout << "Sent with sequence " << result.sequence() << "\n";
}
```

### Streaming Subscriptions

```cpp
// Receive messages from vehicles
client.SubscribeToVehicleMessages(
    [](const std::string& vehicle_id, const std::vector<uint8_t>& payload,
       uint64_t sequence, int64_t timestamp_ms) {
        std::cout << "Message from " << vehicle_id << ": "
                  << payload.size() << " bytes\n";
    });

// Monitor vehicle status
client.SubscribeToVehicleStatus(
    [](const std::string& vehicle_id, vehicle_status_t status, int64_t ts) {
        if (status == vehicle_status_t::ONLINE) {
            std::cout << vehicle_id << " came online\n";
        } else if (status == vehicle_status_t::OFFLINE) {
            std::cout << vehicle_id << " went offline\n";
        }
    });

// Track delivery acknowledgments
client.SubscribeToAcks(
    [](const std::string& vehicle_id, uint64_t sequence) {
        std::cout << "Message " << sequence << " delivered to " << vehicle_id << "\n";
    });

// Cleanup
client.StopSubscriptions();
```

### Status Queries

```cpp
// Health check
bool healthy = client.IsHealthy();

// Query vehicle status
auto [status, last_seen] = client.GetVehicleStatus("VIN123");
if (status == vehicle_status_t::ONLINE) {
    std::cout << "Vehicle online, last seen: " << last_seen << "ms\n";
}

// Get channel info
auto info = client.GetChannelInfo();
std::cout << "Handling content_id=" << info.content_id()
          << " partition=" << info.partition_id()
          << "/" << info.total_partitions() << "\n";

// Statistics
auto stats = client.GetStats();
std::cout << "Sent: " << stats.messages_sent()
          << " Received: " << stats.messages_received() << "\n";
```

---

## Testing

### Integration Tests

The integration tests verify the complete vehicle↔cloud communication:

```bash
# Build tests
cmake --build build --target ifex-cloud-backend-transport-integration-test

# Run tests (starts MQTT container automatically)
./build/cloud/cloud-backend-transport/ifex-cloud-backend-transport-integration-test
```

### Test Coverage (17 tests)

| Category | Test | Description |
|----------|------|-------------|
| **Health** | `BothServicesAreHealthy` | Health checks on both sides |
| **Config** | `ChannelInfoMatches` | Partition configuration |
| **C2V Messages** | `CloudToVehicle_SingleMessage` | Single C2V message delivery |
| | `CloudToVehicle_MultipleMessages` | C2V message ordering (3 messages) |
| **V2C Messages** | `VehicleToCloud_SingleMessage` | Single V2C message delivery |
| | `VehicleToCloud_MultipleMessages` | V2C message ordering (3 messages) |
| **Edge Cases** | `LargePayload` | 64KB payload delivery without truncation |
| | `SendToUnknownVehicle` | API accepts message, vehicle status is UNKNOWN |
| **Status** | `CloudSeesVehicleAsOnline` | Vehicle shows ONLINE after connect |
| **ACKs** | `CloudReceivesAckAfterSend` | Cloud receives ACK after C2V send |
| | `VehicleReceivesAckAfterPublish` | Vehicle receives ACK after V2C publish |
| **Bidirectional** | `BidirectionalMessageExchange` | Simultaneous C2V + V2C both succeed |
| **Stats** | `StatsIncrementOnBothSides` | Statistics counters increase correctly |
| **Reconnect** | `CloudSeesVehicleConnectDisconnectReconnect` | Full ONLINE→OFFLINE→ONLINE cycle |
| | `MessagesDeliveredAfterReconnect` | C2V works after vehicle reconnect |
| | `VehicleCanPublishAfterReconnect` | V2C works after vehicle reconnect |
| **Multi-Vehicle** | `MultipleVehiclesRouteCorrectly` | Correct C2V and V2C routing for 2 vehicles |

### External MQTT Broker

Skip Docker container by setting environment variables:

```bash
MQTT_HOST=192.168.1.100 MQTT_PORT=1883 \
  ./ifex-cloud-backend-transport-integration-test
```

---

## Files

```
cloud-backend-transport/
├── include/
│   ├── cloud_backend_transport_server.hpp   # Server interface
│   └── cloud_backend_transport_client.hpp   # Client API
├── src/
│   ├── cloud_backend_transport_server.cpp   # Server implementation
│   ├── cloud_backend_transport_client.cpp   # Client implementation
│   └── main.cpp                             # Standalone service entry
├── tests/
│   └── cloud_backend_transport_integration_test.cpp
├── CMakeLists.txt
└── README.md
```

---

## See Also

- [Backend Transport Service](../../reference-services/backend-transport/README.md) - Vehicle-side counterpart
- [IFEX Service Architecture](../../docs/ifex-service-architecture.md) - How to build IFEX services
- [Discovery Sync Protocol](../../docs/discovery-sync-protocol.md) - Service registry sync
- [RPC Protocol](../../docs/rpc-protocol.md) - Cloud-initiated method calls
