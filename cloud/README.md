# Cloud Reference Services

This directory contains cloud-side reference implementations for testing and development.

**These are NOT production services.** They are simple implementations used for:
- Integration testing
- Local development
- Demonstrating the cloud-side API contract

## Build

Cloud services are only built when tests are enabled:

```bash
./build.sh --debug --test
```

They are NOT built for cross-compilation targets.

## Contents

| Directory | Description |
|-----------|-------------|
| `ifex/` | IFEX specifications for cloud-side services |
| `cloud-backend-transport/` | MQTT-based cloud↔vehicle transport service |

## Services

### Cloud Backend Transport

Cloud-side counterpart to the vehicle [Backend Transport Service](../reference-services/backend-transport/README.md).

```
Cloud Services (gRPC clients)
        │
        ▼
┌─────────────────────────────────┐
│ CloudBackendTransportServer     │
│   - Send messages to vehicles   │
│   - Receive vehicle messages    │
│   - Track vehicle online status │
└─────────────────────────────────┘
        │ MQTT
        ▼
┌─────────────────────────────────┐
│ Vehicle BackendTransportServer  │
└─────────────────────────────────┘
```

**Features:**
- Bidirectional messaging (C2V and V2C)
- Vehicle online/offline tracking
- Delivery acknowledgments
- Multi-vehicle routing

See [cloud-backend-transport/README.md](cloud-backend-transport/README.md) for full documentation.

## Specifications

- `ifex/cloud-backend-transport-service.yml` - Cloud-side API specification

These specs pair with vehicle-side specs in `reference-services/ifex/`.

## Production Implementations

For production cloud deployments, see `covesa-ifex-offboard-services` which provides:
- Kafka-based message ingestion
- PostgreSQL persistence
- Horizontal scaling with partitioning
- Fleet-wide service discovery
