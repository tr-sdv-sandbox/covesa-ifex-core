# Cloud Backend Transport Service Specification

## Overview

This document describes the cloud-side counterpart to the vehicle-side `backend_transport_service`. Together they form a symmetric pair for vehicle-to-cloud communication.

## Problem Statement

The vehicle-side has a well-defined IFEX spec (`backend-transport-service.yml`), but the cloud side was implementation-defined. This created:

1. **Tight coupling** - Cloud services coupled to Kafka topics, MQTT details
2. **No stable API** - Changing from MQTT→Kafka to MQTT-only requires rewriting consumers
3. **Ad-hoc contract** - Implementation defined behavior rather than spec

## Solution: Paired Specifications

```
┌─────────────────────────────────┐       ┌─────────────────────────────────┐
│  Vehicle Function               │       │  Cloud Function                 │
│  backend_transport_service      │       │  cloud_backend_transport_service│
│  (content_id=201)               │       │  (content_id=201, partition=2)  │
│                                 │       │                                 │
│  publish(payload)          ─────┼──────▶│  on_vehicle_message(            │
│                                 │       │      vehicle_id, payload)       │
│                                 │       │                                 │
│  on_content(payload)       ◀────┼───────│  send_to_vehicle(               │
│                                 │       │      vehicle_id, payload)       │
│                                 │       │                                 │
│  (implicit: my vehicle_id)      │       │  (explicit: which vehicle)      │
└─────────────────────────────────┘       └─────────────────────────────────┘
        ONE vehicle                               MANY vehicles
        ONE content_id                            ONE content_id + partition
```

### Key Design Decisions

1. **Channel-bound content_id + partition** - Each instance is bound to one content_id and one partition
2. **Partitioning for horizontal scaling** - Enables Kafka-like parallel processing of vehicle fleet
3. **Explicit vehicle_id** - Cloud sees fleet, so vehicle_id is explicit parameter
4. **Same sequence semantics** - Per-vehicle monotonic sequences, gaps indicate dropped messages
5. **Copied types** - Shared enums (persistence_t, etc.) are copied, not imported
6. **Optional metadata struct** - Transport-provided fields in single optional struct for efficiency

### Partitioning Model

Each cloud instance is bound to a partition:

```yaml
channel_info_t:
  content_id: 201
  partition_id: 2        # 0 for non-partitioned
  total_partitions: 8    # 1 for non-partitioned
```

| Deployment | partition_id | total_partitions | Use Case |
|------------|--------------|------------------|----------|
| Simple (MQTT-only) | 0 | 1 | Testing, development |
| Scaled (Kafka) | 0-7 | 8 | Production with 8 consumers |

**Partition contract:**
- `on_vehicle_message` → only delivers vehicles in this partition
- `send_to_vehicle` → returns `WRONG_PARTITION` if vehicle not owned
- `get_queue_status` → per-vehicle, but only for this partition
- `get_stats` → statistics for this partition only

### Extensible Metadata

Rather than polluting the core spec with transport-specific fields, we use an optional struct:

```yaml
transport_metadata_t:
  members:
    - originator: string
      mandatory: false
    - message_id: uint64
      mandatory: false
    - received_at_ms: int64
      mandatory: false
    # New fields can be added here without breaking consumers
```

This generates efficient protobuf (single decode) while remaining extensible:
- All fields optional
- Adding new fields is backwards compatible
- Typed access (not map<string, bytes>)

## Specification Files

| File | Location | Purpose |
|------|----------|---------|
| `backend-transport-service.yml` | `reference-services/ifex/` | Vehicle-side spec |
| `cloud-backend-transport-service.yml` | `cloud/ifex/` | Cloud-side spec |

Both specs live in `covesa-ifex-core`. The cloud reference implementation is only built when tests are enabled (not for cross-compilation targets).

## Symmetric Operations

| Vehicle | Cloud | Description |
|---------|-------|-------------|
| `publish(payload)` | `on_vehicle_message(vehicle_id, payload)` | V→C data flow |
| `on_content(payload)` | `send_to_vehicle(vehicle_id, payload)` | C→V data flow |
| `on_ack(sequence)` | `on_ack(vehicle_id, sequence)` | Delivery confirmation |
| LWT publish | `on_vehicle_status(vehicle_id, status)` | Connection status |
| `get_content_id()` | `get_channel_info()` | Channel binding (cloud adds partition) |

## Implementation Bindings

The spec is implementation-agnostic. Example bindings:

### Production: MQTT → Kafka (8 partitions)
```
Vehicle → MQTT v2c/{vid}/{cid} → mqtt_kafka_bridge → Kafka (partitioned by vid)
                                                          │
                    ┌─────────────────────────────────────┼─────────────────────────────────────┐
                    ▼                                     ▼                                     ▼
         Cloud Service (p=0)                   Cloud Service (p=1)              ...    Cloud Service (p=7)
         partition_id=0                        partition_id=1                          partition_id=7
         total_partitions=8                    total_partitions=8                      total_partitions=8
```

Each instance only receives/sends vehicles where `hash(vehicle_id) % 8 == partition_id`.

### Testing: MQTT-only (no partitioning)
```
Vehicle → MQTT v2c/{vid}/{cid} → Cloud Service (partition_id=0, total_partitions=1)
```

Single instance handles all vehicles.

### Future: Direct gRPC
```
Vehicle → gRPC stream → Cloud Service
```

The cloud service code doesn't change - only the transport binding and partition assignment.

