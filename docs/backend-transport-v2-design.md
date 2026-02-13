# Backend Transport — Design Proposal

## Overview

This document proposes the Backend Transport specification for vehicle-to-cloud communication within the IFEX platform. The specification is **language-agnostic** — it defines message semantics, not implementation. Reference implementations in C++ and Rust serve as development tools for E2E testing of components built on top.

### Design Goals

1. **One message contract, multiple access paths** — whether a cloud service uses gRPC or consumes Kafka directly, the semantics (sequences, acks, persistence, queue feedback) are identical
2. **Vehicle side: single interface** — one service, one broker connection, content-id-bound channels
3. **Cloud side: hexagonal architecture** — core domain logic (routing, queuing, status tracking) is independent of transport adapters
4. **Spec-first** — IFEX YAML is the source of truth; implementations are interchangeable
5. **Reference implementations for testing** — not production code; prove the spec works and enable E2E testing of dependent components

---

## Architecture

### Production Topology

```
                              CLOUD
┌──────────────────────────────────────────────────────────────────────┐
│                                                                      │
│  ┌─────────────┐    gRPC     ┌────────────────────────┐             │
│  │ Cloud       │────────────▶│ Cloud Transport        │             │
│  │ Service A   │◀────────────│ gRPC Service           │             │
│  │             │             │                        │             │
│  │ (convenience│             │ Internally produces/   │──┐          │
│  │  / low vol) │             │ consumes Kafka         │  │          │
│  └─────────────┘             └────────────────────────┘  │          │
│                                                          ▼          │
│  ┌─────────────┐             ┌────────────────────────────────┐     │
│  │ Cloud       │────────────▶│                                │     │
│  │ Service B   │◀────────────│         Kafka                  │     │
│  │             │             │   (partitioned by VIN)         │     │
│  │ (high vol / │             │                                │     │
│  │  direct)    │             └───────────────┬────────────────┘     │
│  └─────────────┘                             │                      │
│                              ┌───────────────▼────────────────┐     │
│                              │   Kafka ↔ MQTT Bridge          │     │
│                              │   (ONLY component with         │     │
│                              │    MQTT broker access)         │     │
│                              └───────────────┬────────────────┘     │
└──────────────────────────────────────────────┼──────────────────────┘
                                               │ MQTT
                                        ┌──────▼──────┐
                                        │ MQTT Broker  │
                                        └──────┬──────┘
                                               │
┌──────────────────────────────────────────────┼──────────────────────┐
│                          VEHICLE             │                      │
│                                              │                      │
│  ┌─────────────┐    gRPC     ┌───────────────▼────────────────┐     │
│  │ IFEX        │────────────▶│ Backend Transport              │     │
│  │ Service     │◀────────────│ Service                        │     │
│  └─────────────┘             │ (single MQTT connection,       │     │
│                              │  per-content-id queues)        │     │
│  ┌─────────────┐    gRPC     │                                │     │
│  │ IFEX        │────────────▶│                                │     │
│  │ Service     │◀────────────│                                │     │
│  └─────────────┘             └────────────────────────────────┘     │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### Key Architectural Decisions

**1. Kafka is the production backbone**

All cloud-side message flow goes through Kafka. The gRPC cloud transport service is a typed facade — it produces to and consumes from Kafka internally. This means:
- Kafka topics are the source of truth for message ordering and persistence
- Horizontal scaling is achieved via Kafka consumer groups and partitioning
- The gRPC service adds no independent state — it's stateless

**2. MQTT broker is encapsulated**

In production, the MQTT broker is an internal detail of the Kafka↔MQTT bridge. No cloud service interacts with MQTT directly. This keeps the broker as a dedicated vehicle-facing transport layer with no competing consumers.

**3. Two cloud access paths, one contract**

| Path | Use case | How it works |
|------|----------|-------------|
| **gRPC** | Convenience, low-to-medium volume, typed API | Produces/consumes Kafka internally |
| **Kafka direct** | High volume, streaming analytics, custom consumers | Same topics, same message format |

Both paths observe identical semantics: same sequence numbering, same ack behavior, same persistence guarantees. The spec defines these semantics once.

The gRPC write path also goes through Kafka in production — it is not a shortcut. This ensures a single message flow regardless of how the message was submitted.

**4. Vehicle side is simple**

One interface. One broker connection shared across all on-vehicle clients. Per-content-id channels. No hexagonal layering needed — the vehicle has exactly one deployment topology.

### E2E Test Topology

For development and E2E testing, the full Kafka infrastructure is not needed:

```
Cloud Service ──gRPC──▶ Cloud Transport (ref impl) ──MQTT──▶ Broker ◀──MQTT── Vehicle Transport (ref impl)
```

The reference implementation backs the same gRPC API with direct MQTT instead of Kafka. Same interface, same semantics, simpler infrastructure.

---

## Message Semantics Contract

These semantics are invariant across all access paths (vehicle gRPC, cloud gRPC, cloud Kafka direct). They are what the spec defines.

### 1. Content ID Binding

Every message belongs to a **content ID** — a numeric identifier for the type of content (e.g., 200=RPC, 201=discovery sync, 202=scheduler sync).

- **Vehicle side**: Each client handle is bound to exactly one content ID at construction. The client cannot publish to a different content ID. This prevents accidental cross-topic writes.
- **Cloud side (gRPC)**: Content ID is explicit in each request (the cloud service handles multiple content IDs).
- **Cloud side (Kafka)**: Content ID is encoded in the Kafka topic name.

### 2. Sequence Numbers

Every published message receives a **server-assigned, monotonically increasing sequence number** scoped to:
- **Vehicle side**: per content_id
- **Cloud side**: per (vehicle_id, content_id) pair

Sequence numbers enable:
- **Non-blocking publish**: Returns immediately with the assigned sequence
- **Delivery confirmation**: `on_ack` events reference the sequence number
- **Gap-based failure detection**: If sequence 5 is acked but 4 is not, message 4 failed
- **Ordering verification**: Consumers can detect out-of-order delivery

### 3. Delivery Acknowledgment

Acks are **success-only**. A message is considered:
- **Delivered**: When an `on_ack` event carries its sequence number
- **Failed**: When a later sequence number is acked but this one was not (gap detection)
- **Pending**: When no ack has been received yet

There is no explicit failure notification. Gaps in acked sequences are the failure signal.

### 4. Persistence Levels

All persistence levels preserve FIFO ordering within a content_id (vehicle) or (vehicle_id, content_id) pair (cloud).

| Level | Queued | Retry on failure | Survives shutdown |
|-------|--------|-----------------|------------------|
| `BEST_EFFORT` | Yes (for ordering) | No | No |
| `VOLATILE` | Yes | Yes, until delivered | No (lost on any shutdown) |
| `DURABLE` | Yes | Yes, until delivered | Yes (graceful shutdown only) |

`DURABLE` is explicitly not crash-safe — this is a deliberate choice to protect embedded flash from constant writes. It persists on signal-handler-triggered graceful shutdown only.

### 5. Queue Feedback (Adaptive Throttling)

Every publish response includes the current `queue_level`. Queue level changes are also broadcast via `on_queue_status_changed` events.

| Level | Fill % | Recommended action |
|-------|--------|-------------------|
| `EMPTY` | 0% | — |
| `LOW` | < 25% | — |
| `NORMAL` | 25-50% | — |
| `HIGH` | 50-75% | Consider throttling low-priority data |
| `CRITICAL` | 75-95% | Throttle; only high-priority accepted |
| `FULL` | > 95% | Publish returns `QUEUE_FULL` |

### 6. Vehicle Online/Offline Status

The vehicle's connection state is observable from the cloud side:

- **Detection mechanism**: MQTT Last Will and Testament (LWT) with retained messages
- **Topic**: `v2c/{vehicle_id}/is_online` — payload `"1"` (online) or `"0"` (offline)
- **Graceful disconnect**: Vehicle publishes `"0"` before disconnecting
- **Crash/network loss**: Broker publishes LWT `"0"` automatically
- **Cloud observation**: Via `on_vehicle_status` event (gRPC) or Kafka topic (direct)

### 7. Partitioning (Cloud Side)

Cloud instances can be horizontally scaled via partitioning:

- Partition assignment: `hash(vehicle_id) % total_partitions == partition_id`
- Each instance only handles vehicles in its partition
- In non-partitioned deployments: `partition_id=0`, `total_partitions=1`
- Partition-aware operations return `WRONG_PARTITION` for misrouted requests

In Kafka deployments, Kafka consumer group partitions align with transport partitions.

---

## Cloud-Side Hexagonal Architecture

The cloud transport's core logic is independent of how messages arrive and depart.

### Ports

**Primary Ports (Driving — inbound to the core):**

| Port | Purpose | Adapters |
|------|---------|----------|
| `CloudTransportAPI` | Cloud services send/receive messages | gRPC adapter, Kafka consumer adapter |

**Secondary Ports (Driven — outbound from the core):**

| Port | Purpose | Adapters |
|------|---------|----------|
| `VehicleMessageSink` | Deliver message toward vehicle | Kafka producer (prod), MQTT direct (test) |
| `VehicleMessageSource` | Receive messages from vehicles | Kafka consumer (prod), MQTT subscriber (test) |
| `VehicleStatusStore` | Track vehicle online/offline state | Database (prod), in-memory (test) |

### Core Domain

The core owns:
- **Per-vehicle queue management** — FIFO ordering, capacity tracking, queue level computation
- **Sequence number assignment** — per (vehicle_id, content_id), monotonically increasing
- **Partition ownership** — determines which vehicles this instance handles
- **Persistence level enforcement** — retry policy, pruning policy
- **Delivery tracking** — correlates acks with published sequences
- **Adaptive throttling** — queue level transitions, backpressure signals

The core does NOT own:
- How messages reach Kafka or MQTT (adapter concern)
- How partitions are assigned (Kafka consumer groups vs static config — adapter concern)
- Wire format (protobuf for gRPC, defined schema for Kafka — adapter concern)
- Vehicle status detection mechanism (LWT vs heartbeat — adapter concern)

### Adapter Configurations

**Production:**

```
gRPC Adapter ──▶ Core ──▶ Kafka Producer Adapter ──▶ Kafka
                  ▲
Kafka Consumer Adapter ◀── Kafka ◀── Kafka↔MQTT Bridge ◀── MQTT Broker ◀── Vehicle
```

**E2E Test (Reference Implementation):**

```
gRPC Adapter ──▶ Core ──▶ MQTT Direct Adapter ──▶ MQTT Broker ◀── Vehicle
                  ▲
MQTT Subscriber Adapter ◀── MQTT Broker ◀── Vehicle
```

The core domain code is identical in both configurations. Only the adapters change.

---

## Specification Files

### Vehicle-Side Spec

**File**: `reference-specs/backend-transport-v2/vehicle/backend-transport-service.ifex.yml`

Defines:
- Single namespace `transport`
- Enumerations: `connection_state_t`, `publish_status_t`, `queue_level_t`, `disconnect_reason_t`, `persistence_t`
- Structs: `publish_request_t`, `publish_response_t`, `delivery_ack_t`, `connection_status_t`, `queue_status_t`, `transport_stats_t`, `content_message_t`
- Methods: `publish`, `get_connection_status`, `get_queue_status`, `get_stats`, `healthy`, `get_content_id`
- Events: `on_content`, `on_ack`, `on_connection_changed`, `on_queue_status_changed`

The vehicle side has a single interface with a content-id-bound channel model. Each on-vehicle service receives a handle tied to one content ID and cannot accidentally publish to the wrong topic.

### Cloud-Side Spec (gRPC API)

**File**: `reference-specs/backend-transport-v2/cloud/cloud-backend-transport-service.ifex.yml`

Defines the gRPC service interface that cloud services consume. Key differences from the vehicle-side:
- `vehicle_id` is explicit (cloud handles a fleet)
- Content ID is explicit per request (cloud routes multiple content types)
- Partition-aware: `get_channel_info()` returns partition assignment
- Fleet operations: `list_vehicles()`, `get_vehicle_status()`
- Subscription filter: `on_vehicle_message` takes a content_id filter

### Cloud-Side Kafka Message Format

**File**: `reference-specs/backend-transport-v2/cloud/kafka-message-format.md`

Defines the Kafka topic structure and message schemas for direct consumers/producers. This is not an IFEX YAML file (Kafka is not an RPC interface), but a message format specification:

**Topics:**

| Topic pattern | Direction | Partitioning |
|---------------|-----------|-------------|
| `v2c.{content_id}` | Vehicle → Cloud | By `vehicle_id` (message key) |
| `c2v.{content_id}` | Cloud → Vehicle | By `vehicle_id` (message key) |
| `vehicle.status` | Status events | By `vehicle_id` (message key) |
| `transport.acks` | Delivery acks | By `vehicle_id` (message key) |

**Message schemas** use the same type definitions as the gRPC spec (same enumerations, same structs), serialized as Protocol Buffers. A cloud service consuming `v2c.201` directly from Kafka sees the same `vehicle_message_t` structure it would receive via the gRPC `on_vehicle_message` stream.

---

## Reference Implementation

### Purpose

The reference implementations exist to:
1. **Prove the spec** — demonstrate that the semantics are implementable and consistent
2. **Enable E2E testing** — components building on top of backend transport can test against a real (but simple) implementation
3. **Serve as example code** — show how to implement the spec in C++ and Rust

They are NOT production code. Specifically:
- No Kafka integration (direct MQTT only)
- No horizontal scaling
- No database-backed vehicle status
- Single-process cloud side

### Scope

| Component | C++ | Rust |
|-----------|-----|------|
| Vehicle-side transport service | Yes | Yes |
| Cloud-side transport service (gRPC API, MQTT-backed) | Yes | Yes |
| Client libraries | Yes | Yes |
| Integration tests (MQTT) | Yes | Yes |

### What Each Reference Implementation Covers

- **Vehicle side**: gRPC service, MQTT client wrapper, per-content-id message queues
- **Cloud side**: gRPC service, direct MQTT adapter (test adapter), vehicle status tracking, message routing (core domain logic)

Both C++ and Rust implementations cover the same components. The Rust cloud-side implementation naturally expresses the hexagonal ports as traits, while C++ uses abstract base classes.

---

## MQTT Topic Structure

```
Vehicle → Cloud:
  v2c/{vehicle_id}/{content_id}         e.g., v2c/WDB12345/201

Cloud → Vehicle:
  c2v/{vehicle_id}/{content_id}         e.g., c2v/WDB12345/201

Vehicle Status (LWT):
  v2c/{vehicle_id}/is_online            Payload: "1" or "0", retained, QoS 1
```

---

## Implementation Roadmap

1. **Spec finalization** — Finalize IFEX YAML specs and Kafka message format (this document + spec files). No code.
2. **Reference impl scaffolding** — Create `reference-impl/` directory structure with C++ and Rust projects.
3. **C++ reference implementation** — Implement vehicle-side and cloud-side (MQTT-backed) in C++.
4. **Rust reference implementation** — Implement vehicle-side and cloud-side (MQTT-backed) in Rust.
5. **Kafka message format finalization** — Finalize Kafka topic/message format specification for production deployments.

---

## Open Questions

1. **Kafka message serialization**: Should Kafka messages use Protocol Buffers (consistent with gRPC) or a more Kafka-native format (Avro with Schema Registry)?
2. **Rust gRPC framework**: `tonic` is the de-facto standard. Any reason to consider alternatives?
4. **Rust MQTT library**: `rumqttc` vs `paho-mqtt-rust`. `rumqttc` is pure Rust and more idiomatic; `paho-mqtt-rust` wraps the C library (consistent with C++ using mosquitto).
5. **Build integration**: Should the Rust reference impl be built by CMake (via corrosion) or independently via Cargo?
