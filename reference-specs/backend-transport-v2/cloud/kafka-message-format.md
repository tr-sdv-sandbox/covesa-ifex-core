# Kafka Message Format Specification

## Overview

This document defines the Kafka topic structure and message schemas for cloud services that consume/produce directly from Kafka, bypassing the gRPC Cloud Transport service. The message semantics are identical to the gRPC API — this is an alternative access path, not a different contract.

**When to use Kafka directly vs gRPC:**

| Access path | Use case |
|-------------|----------|
| gRPC Cloud Transport | Convenience, typed API, low-to-medium throughput |
| Kafka direct | High throughput, streaming analytics, custom consumer groups |

Both paths use the same underlying Kafka topics. The gRPC service is a facade over these topics.

---

## Topic Structure

### Vehicle-to-Cloud (v2c)

| Topic | Key | Description |
|-------|-----|-------------|
| `v2c.{content_id}` | `vehicle_id` (string) | Messages from vehicles for a given content type |

Examples:
- `v2c.200` — RPC messages from vehicles (key: `WDB12345`)
- `v2c.201` — Discovery sync messages (key: `WDB12345`)
- `v2c.202` — Scheduler sync messages (key: `WDB12345`)

**Partitioning**: By message key (`vehicle_id`). All messages from a given vehicle land in the same partition, preserving per-vehicle ordering.

### Cloud-to-Vehicle (c2v)

| Topic | Key | Description |
|-------|-----|-------------|
| `c2v.{content_id}` | `vehicle_id` (string) | Messages destined for vehicles |

Examples:
- `c2v.200` — RPC responses to vehicles (key: `WDB12345`)
- `c2v.201` — Discovery sync responses (key: `WDB12345`)

**Partitioning**: By message key (`vehicle_id`). The Kafka-MQTT bridge consumes these and publishes to `c2v/{vehicle_id}/{content_id}` via MQTT.

### Vehicle Status

| Topic | Key | Description |
|-------|-----|-------------|
| `vehicle.status` | `vehicle_id` (string) | Online/offline status events |

Produced by the Kafka-MQTT bridge when it observes LWT or online messages on `v2c/{vehicle_id}/is_online`.

### Delivery Acknowledgments

| Topic | Key | Description |
|-------|-----|-------------|
| `transport.acks` | `vehicle_id` (string) | Delivery confirmation events |

Produced by the Kafka-MQTT bridge when MQTT QoS 1 PUBACK is received for a c2v message.

---

## Message Schemas

The wire format on Kafka is an internal detail. Consumers access messages through a `decode()` function provided by a client library — the serialization format behind that boundary can change without affecting consumers. What matters is the logical structure of the decoded messages, which must match the types defined in `cloud-backend-transport-service.ifex.yml`.

The protobuf schemas below are a **proposed** wire format for discussion. Protocol Buffers are a natural fit (consistent with gRPC, compact, schema-evolvable), but alternatives (e.g., Avro with Schema Registry) could serve the same role as long as the decode boundary is maintained.

### v2c Message (Vehicle → Cloud)

Corresponds to `vehicle_message_t` in the IFEX spec.

```protobuf
message VehicleMessage {
  string vehicle_id = 1;
  bytes payload = 2;
  uint64 sequence = 3;        // Vehicle's outbound sequence (per content_id)
  int64 timestamp_ms = 4;

  // Optional transport metadata
  TransportMetadata metadata = 10;
}

message TransportMetadata {
  string originator = 1;      // e.g., "backend_transport"
  uint64 message_id = 2;      // Transport-layer message ID
  int64 received_at_ms = 3;   // When ingestion layer received it
  string source_topic = 4;    // Original MQTT topic (debugging)
}
```

**Kafka record:**
- Topic: `v2c.{content_id}`
- Key: `vehicle_id` (UTF-8 string)
- Value: `VehicleMessage` (protobuf-encoded)
- Headers: (none required)

### c2v Message (Cloud → Vehicle)

Corresponds to `send_request_t` in the IFEX spec.

```protobuf
message CloudToVehicleMessage {
  string vehicle_id = 1;
  uint32 content_id = 2;
  bytes payload = 3;
  Persistence persistence = 4;
  uint64 sequence = 5;         // Assigned by the producing service or gRPC facade
}

enum Persistence {
  BEST_EFFORT = 0;
  VOLATILE = 1;
  DURABLE = 2;
}
```

**Kafka record:**
- Topic: `c2v.{content_id}`
- Key: `vehicle_id` (UTF-8 string)
- Value: `CloudToVehicleMessage` (protobuf-encoded)
- Headers: (none required)

### Vehicle Status Event

Corresponds to `vehicle_status_event_t` in the IFEX spec.

```protobuf
message VehicleStatusEvent {
  string vehicle_id = 1;
  VehicleStatus status = 2;
  int64 timestamp_ms = 3;
  int64 last_seen_ms = 4;     // Optional: last message from this vehicle
}

enum VehicleStatus {
  UNKNOWN = 0;
  ONLINE = 1;
  OFFLINE = 2;
}
```

**Kafka record:**
- Topic: `vehicle.status`
- Key: `vehicle_id` (UTF-8 string)
- Value: `VehicleStatusEvent` (protobuf-encoded)

### Delivery Acknowledgment

Corresponds to `delivery_ack_t` in the IFEX spec.

```protobuf
message DeliveryAck {
  string vehicle_id = 1;
  uint64 sequence = 2;
}
```

**Kafka record:**
- Topic: `transport.acks`
- Key: `vehicle_id` (UTF-8 string)
- Value: `DeliveryAck` (protobuf-encoded)

---

## Consumer Group Conventions

| Consumer group pattern | Purpose |
|----------------------|---------|
| `cloud-transport-grpc` | The gRPC Cloud Transport service instances |
| `{service-name}-{content_id}` | Direct Kafka consumers for a specific content type |

Example: A discovery sync service consuming `v2c.201` directly would use consumer group `discovery-sync-201`. This is independent of the gRPC transport service.

---

## Partition Alignment

In production with N partitions:

- Kafka topics are configured with N partitions
- The gRPC Cloud Transport runs N instances, each assigned one partition
- Partition assignment: `hash(vehicle_id) % N == partition_id`
- Kafka's default partitioner (murmur2 hash of key) handles this automatically when `vehicle_id` is the message key

For direct Kafka consumers, partition assignment is handled by the Kafka consumer group protocol. The `WRONG_PARTITION` concept from the gRPC API does not apply — Kafka consumer groups handle routing natively.

---

## Ordering Guarantees

- **Per-vehicle ordering is guaranteed** within a topic partition (all messages from a vehicle go to the same partition because `vehicle_id` is the key)
- **Cross-vehicle ordering is NOT guaranteed** (messages from different vehicles may be in different partitions)
- **Cross-content-id ordering is NOT guaranteed** (different content IDs are different topics)

This matches the gRPC API semantics: ordering is per (vehicle_id, content_id).

---

## Relationship to gRPC API

The gRPC Cloud Transport service is a consumer/producer on these same topics:

| gRPC method/event | Kafka interaction |
|-------------------|-------------------|
| `send_to_vehicle(request)` | Produces to `c2v.{content_id}` |
| `on_vehicle_message(filter)` | Consumes from `v2c.{filter.content_id}` |
| `on_ack(ack)` | Consumes from `transport.acks` |
| `on_vehicle_status(event)` | Consumes from `vehicle.status` |

A cloud service using the gRPC API and a service consuming Kafka directly see the same messages with the same structure. The choice is purely about access convenience vs throughput.
