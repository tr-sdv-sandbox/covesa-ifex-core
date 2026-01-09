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
| `cloud-backend-transport/` | Simple MQTT-only backend transport (TODO) |

## Specifications

- `ifex/cloud-backend-transport-service.yml` - Cloud-side counterpart to vehicle `backend-transport-service.yml`

These specs pair with vehicle-side specs in `reference-services/ifex/`.

## Production Implementations

For production cloud deployments, see `covesa-ifex-offboard-services` which provides:
- Kafka-based message ingestion
- PostgreSQL persistence
- Scalable fleet management
