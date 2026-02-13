# ADR: Backend Transport Architecture

**Status**: Proposed
**Date**: 2026-02-13
**Authors**: svante karlsson & håkan nilson

---

## Context

The IFEX platform needs a transport layer for bidirectional vehicle-to-cloud communication. On-vehicle IFEX services (climate, defrost, scheduler sync, discovery sync, RPC bridge, etc.) need to exchange messages with cloud services reliably, without coupling to a specific transport protocol.

The transport must serve two very different environments:

- **Vehicle side**: Resource-constrained, single vehicle, intermittent connectivity, embedded flash that shouldn't be written to constantly
- **Cloud side**: Fleet-scale (potentially millions of vehicles), high throughput, horizontally scalable, multiple teams consuming the same data streams

Additionally, the specification must be **language-agnostic** — it defines a contract, not an implementation. Reference implementations in C++ and Rust exist solely for development-time E2E testing of components that build on top of the transport.

---

## Decision

### D1: Language-Agnostic Spec with Reference Implementations

The Backend Transport is defined as an IFEX YAML specification. The spec describes message semantics (types, methods, events, behavioral guarantees) without prescribing implementation language, framework, or wire protocol.

Reference implementations in C++ and Rust prove the spec is implementable and provide E2E test infrastructure. They are not production code — they use direct MQTT instead of Kafka, run single-process, and have no database-backed state.

**Rationale**: Production implementations will vary by OEM and deployment context. A spec-first approach allows any team to implement the transport in their stack while maintaining interoperability. The reference implementations catch spec ambiguities early and give dependent component teams something to test against.

### D2: Single Vehicle-Side Interface

The vehicle side exposes one interface. Multiple on-vehicle services share a single broker connection through the transport service. Each service receives a handle bound to a single **content ID** — a numeric identifier for the type of content (e.g., 200=RPC, 201=discovery sync, 202=scheduler sync).

The content-id-bound channel model means:
- A service cannot accidentally publish to the wrong topic
- Content ID is implicit in the handle, not a parameter on every call
- The transport service manages one broker connection for all clients

**Rationale**: The vehicle has exactly one deployment topology. There is no need for adapter abstractions or pluggable backends on the vehicle side — the complexity is not justified. A single interface keeps the on-vehicle footprint small and the API simple for service authors.

### D3: Hexagonal Architecture for the Cloud Side

The cloud-side transport uses a hexagonal (ports and adapters) architecture. Core domain logic — per-vehicle queue management, sequence number assignment, partition ownership, persistence enforcement, delivery tracking, adaptive throttling — is independent of how messages arrive and depart.

**Ports:**

| Port | Direction | Purpose |
|------|-----------|---------|
| `CloudTransportAPI` | Primary (driving) | Cloud services send/receive messages |
| `VehicleMessageSink` | Secondary (driven) | Deliver messages toward vehicles |
| `VehicleMessageSource` | Secondary (driven) | Receive messages from vehicles |
| `VehicleStatusStore` | Secondary (driven) | Track vehicle online/offline state |

**Adapter configurations:**

| Deployment | Primary adapter | Secondary adapters |
|------------|----------------|-------------------|
| Production | gRPC service, Kafka consumer | Kafka producer, database |
| E2E test (reference impl) | gRPC service | Direct MQTT, in-memory status |

The core domain code is identical across configurations. Only the adapters change.

**Rationale**: The cloud side genuinely has multiple deployment topologies (Kafka-backed production vs MQTT-only testing). The core logic (queuing, sequencing, throttling) is the same in both — only the transport binding differs. Hexagonal architecture makes this a configuration choice rather than a code fork.

### D4: Kafka as the Production Cloud Backbone

In production, all cloud-side message flow goes through Kafka. The gRPC Cloud Transport service is a stateless typed facade — it produces to and consumes from Kafka internally. The gRPC write path does not bypass Kafka; it goes through it.

```
Cloud Service ──gRPC──▶ Cloud Transport ──produces──▶ Kafka ──▶ Kafka-MQTT Bridge ──▶ MQTT ──▶ Vehicle
```

**Rationale**: Kafka provides durable, partitioned, replayable message streams. Routing all writes through Kafka (even from the gRPC facade) ensures a single source of truth for message ordering. This avoids split-brain scenarios where gRPC and Kafka paths diverge.

### D5: Two Cloud Access Paths, One Message Contract

Cloud services can access transport via two paths:

| Path | Use case | Mechanism |
|------|----------|-----------|
| gRPC Cloud Transport API | Convenience, typed, low-to-medium volume | Facade over Kafka |
| Kafka direct | High volume, streaming, custom consumers | Same topics and message format |

Both paths observe **identical semantics**: same sequence numbering, same ack behavior, same persistence guarantees, same message structure. The spec defines these semantics once.

The Kafka wire format is an internal detail behind a `decode()` boundary in the client library. The serialization format can change without affecting consumers. Protocol Buffers are proposed for discussion (compact, schema-evolvable, consistent with gRPC), but the decode boundary is what's fixed, not the wire format.

**Rationale**: Services processing millions of messages per day (telemetry, analytics) should not pay gRPC overhead per message. Direct Kafka access eliminates the intermediary for high-throughput consumers. The cost is maintaining two access paths, but the message contract is defined once — only the transport binding differs.

### D6: MQTT Broker Encapsulated Behind the Kafka-MQTT Bridge

In production, the MQTT broker is an internal implementation detail. Only the Kafka-MQTT bridge component interacts with the broker. No cloud service connects to MQTT directly.

**Rationale**: The broker is a vehicle-facing transport endpoint. Allowing arbitrary cloud services to subscribe to MQTT topics creates competing consumers, unpredictable load on the broker, and coupling to MQTT topic structure. Encapsulating the broker behind the bridge keeps the vehicle-facing path clean and makes it possible to change the vehicle transport protocol (e.g., to QUIC or direct TLS) without affecting cloud consumers.

### D7: Content-ID-Bound Channels

Every message belongs to a content ID. On the vehicle side, each client handle is bound to exactly one content ID at construction — the client cannot change it or publish to another.

On the cloud side (gRPC), content ID is explicit in each request. On the cloud side (Kafka), content ID is encoded in the topic name (`v2c.{content_id}`, `c2v.{content_id}`).

**Rationale**: Content IDs isolate message streams (RPC, discovery sync, scheduler sync are separate channels). The vehicle-side binding prevents accidental cross-topic writes — a common source of bugs in pub/sub systems. The cloud side needs explicit content IDs because a single cloud service instance may handle multiple content types.

### D8: Server-Assigned Sequence Numbers with Gap-Based Failure Detection

Every published message receives a server-assigned, monotonically increasing sequence number. Scoped to:
- Vehicle: per content_id
- Cloud: per (vehicle_id, content_id)

Delivery confirmations (`on_ack`) reference the sequence number. Acks are success-only — there is no explicit failure notification. A gap in acked sequences (sequence 5 acked, 4 not) indicates message 4 failed.

**Rationale**: Non-blocking publish requires decoupling submission from delivery. Server-assigned sequences (rather than client-assigned) eliminate coordination between clients. Gap-based failure detection avoids the complexity of explicit failure callbacks and timeouts — the absence of an ack is the signal. This model is simple to implement correctly across languages.

### D9: Three Persistence Levels (Not Crash-Safe)

| Level | Retry | Survives shutdown |
|-------|-------|------------------|
| `BEST_EFFORT` | No | No |
| `VOLATILE` | Yes, until delivered | No |
| `DURABLE` | Yes, until delivered | Graceful shutdown only |

All levels preserve FIFO ordering. `DURABLE` explicitly does not provide crash safety — it persists state only on signal-handler-triggered graceful shutdown.

**Rationale**: Crash-safe persistence requires constant disk writes (WAL, fsync). On embedded vehicle hardware with flash storage, this causes premature wear. `DURABLE` protects against planned restarts (software updates, service cycling) without the flash wear of continuous writes. The trade-off is accepted: a hard crash may lose in-flight durable messages.

### D10: Adaptive Throttling via Queue Level Feedback

Every publish response includes the current queue fill level (`EMPTY` through `FULL`). Level changes are also broadcast via `on_queue_status_changed` events.

| Level | Fill % | Guidance |
|-------|--------|----------|
| `HIGH` | 50-75% | Consider throttling low-priority data |
| `CRITICAL` | 75-95% | Throttle; only high-priority accepted |
| `FULL` | > 95% | Publish returns `QUEUE_FULL`, message rejected |

**Rationale**: Vehicle connectivity is intermittent. When the vehicle is disconnected, messages queue up. Without feedback, clients have no way to adapt — they either overrun the queue or need arbitrary rate limits. Queue level feedback lets clients make informed decisions (e.g., drop telemetry but keep safety-critical messages) based on actual queue state.

### D11: Vehicle Online/Offline Detection via MQTT LWT

Vehicle status is detected using MQTT Last Will and Testament (LWT):

- On connect: Set LWT to `"0"` on `v2c/{vehicle_id}/is_online` (retained, QoS 1), then publish `"1"`
- On graceful disconnect: Publish `"0"`, then disconnect
- On crash/network loss: Broker automatically publishes LWT `"0"`

Cloud observes status via `on_vehicle_status` event (gRPC) or `vehicle.status` Kafka topic (direct).

**Rationale**: LWT is a standard MQTT feature that handles both graceful and ungraceful disconnects without requiring a heartbeat mechanism. Retained messages ensure new cloud subscribers immediately know the current state of every vehicle. The vehicle side needs zero additional logic beyond setting the LWT at connection time.

### D12: Cloud-Side Horizontal Scaling via Partitioning

Cloud instances are horizontally scaled via VIN-based partitioning:

- Partition assignment: `hash(vehicle_id) % total_partitions == partition_id`
- Each instance handles only its partition's vehicles
- Non-partitioned deployments: `partition_id=0`, `total_partitions=1`
- gRPC API returns `WRONG_PARTITION` for misrouted requests

In Kafka deployments, Kafka consumer group partitions align with transport partitions.

**Rationale**: Fleet-scale processing requires horizontal scaling. VIN-based partitioning ensures all messages for a given vehicle are handled by the same instance, preserving per-vehicle state (queues, sequences, status tracking) without distributed coordination. Kafka's native partitioning aligns naturally with this model.

---

## MQTT Topic Structure

```
Vehicle → Cloud:    v2c/{vehicle_id}/{content_id}
Cloud → Vehicle:    c2v/{vehicle_id}/{content_id}
Vehicle Status:     v2c/{vehicle_id}/is_online       (retained, QoS 1)
```

## Kafka Topic Structure

| Topic | Key | Direction |
|-------|-----|-----------|
| `v2c.{content_id}` | `vehicle_id` | Vehicle → Cloud |
| `c2v.{content_id}` | `vehicle_id` | Cloud → Vehicle |
| `vehicle.status` | `vehicle_id` | Status events |
| `transport.acks` | `vehicle_id` | Delivery confirmations |

All topics partitioned by `vehicle_id` key for per-vehicle ordering.

---

## Consequences

### Positive

- **Language freedom**: Any team can implement the spec in their stack (Java cloud services, C++ vehicle, Rust edge gateway, etc.)
- **E2E testability**: Reference implementations let component teams test against a real transport without Kafka infrastructure
- **Throughput flexibility**: High-volume cloud consumers bypass gRPC overhead by consuming Kafka directly
- **Clean vehicle-facing path**: MQTT broker encapsulation prevents cloud services from coupling to the vehicle transport protocol
- **Simple vehicle side**: Single interface, no framework overhead, content-id binding prevents misuse
- **Offline resilience**: Queue feedback + persistence levels let vehicle-side services adapt to connectivity loss
- **Independent scaling**: Cloud transport scales horizontally without distributed coordination

### Negative

- **Two cloud access paths to maintain**: gRPC API and Kafka message format must stay in sync. Mitigated by the `decode()` boundary — the Kafka format is internal and can be changed independently of the gRPC contract.
- **Reference implementations in two languages**: C++ and Rust reference impls both need to be kept correct against the spec. This is the cost of language-agnostic design.
- **LWT limitations**: MQTT LWT doesn't provide sub-second disconnect detection. Acceptable for vehicle use cases where minute-level detection is sufficient.
- **No crash-safe persistence**: `DURABLE` only survives graceful shutdown. Hard crashes lose in-flight messages. Accepted trade-off for flash protection.
- **Kafka dependency in production**: The cloud side requires Kafka infrastructure. Not avoidable for fleet-scale workloads, but the reference implementations prove the spec works without it.

### Neutral

- **Kafka wire format is not fixed**: The `decode()` boundary means the serialization format (protobuf, Avro, etc.) can be decided later without affecting the spec or consumers. This is intentionally deferred.
- **Partitioning details are adapter-level**: How partitions are assigned (Kafka consumer groups, static config, Kubernetes operator) is an implementation choice, not a spec concern.

---

## Specification Files

| File | Purpose |
|------|---------|
| `reference-specs/backend-transport-v2/vehicle/backend-transport-service.ifex.yml` | Vehicle-side interface spec |
| `reference-specs/backend-transport-v2/cloud/cloud-backend-transport-service.ifex.yml` | Cloud-side gRPC API spec |
| `reference-specs/backend-transport-v2/cloud/kafka-message-format.md` | Kafka topic structure and proposed message schemas |
| `docs/backend-transport-v2-design.md` | Design proposal (architecture, semantics, reference impl scope) |

---

## Open Questions

1. **Kafka message serialization**: Protocol Buffers (consistent with gRPC) vs Avro with Schema Registry (Kafka-native)? The `decode()` boundary makes this a deferred decision.
2. **Rust gRPC framework**: `tonic` is the de-facto choice. Any constraints that favor an alternative?
3. **Rust MQTT library**: `rumqttc` (pure Rust, idiomatic) vs `paho-mqtt-rust` (wraps C library, consistent with C++ using mosquitto)?
4. **Build integration**: Rust reference impl built by CMake (via corrosion) or independently via Cargo?
