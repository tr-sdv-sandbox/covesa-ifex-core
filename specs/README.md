# IFEX Specifications

This directory contains the authoritative specifications for IFEX services and protocols.

## Directory Structure

```
specs/
├── vehicle/              # Vehicle-side service APIs (IFEX)
│   ├── scheduler-service.ifex.yml
│   ├── discovery-service.ifex.yml
│   ├── dispatcher-service.ifex.yml
│   └── backend-transport-service.ifex.yml
│
├── cloud/                # Cloud-side service APIs (IFEX)
│   ├── cloud-scheduler-service.ifex.yml
│   ├── cloud-discovery-service.ifex.yml
│   ├── cloud-dispatcher-service.ifex.yml
│   └── cloud-backend-transport-service.ifex.yml
│
└── protocols/            # Internal transfer protocols (source of truth)
    ├── scheduler-sync-protocol-v2.md
    ├── scheduler-sync-protocol-v3.md
    ├── discovery-sync-protocol.md
    └── dispatcher-protocol.md
```

## Specification Types

### IFEX Service Specs (`*.ifex.yml`)

Define service APIs (methods, types, events). These generate:
- Protobuf definitions
- gRPC stubs
- Client libraries

Example:
```yaml
name: scheduler_service
methods:
  - name: create_job
    input:
      - name: job
        datatype: job_create_t
    output:
      - name: success
        datatype: boolean
```

### Protocol Specs (`*-protocol.md`)

Define wire formats and state machines for internal communication. These are the **source of truth** - proto files are generated from them (via LLM or manual transcription).

| Protocol | Content ID | Purpose |
|----------|------------|---------|
| Dispatcher | 200 | Cloud-to-vehicle RPC |
| Discovery Sync | 201 | Service registry sync |
| Scheduler Sync | 202 | Job state sync |

## Code Generation

```bash
# Generate protos from IFEX specs
./generate_proto.sh

# Output goes to:
# - proto/ifex-generated/*.proto
# - proto/ifex-generated/python/*.py
```

## Relationship to Reference Implementations

The specs define **what** services do. Reference implementations in `reference-services/` and `cloud/` show **how** to implement them.

```
specs/vehicle/scheduler-service.ifex.yml  →  reference-services/scheduler/
specs/cloud/cloud-scheduler-service.ifex.yml  →  cloud/cloud-scheduler-service/
specs/protocols/scheduler-sync-v2.md  →  Used by both sides
```

## Legacy Locations (Deprecated)

The following locations are deprecated. Use `specs/` instead:
- `reference-services/ifex/*.yml` → `specs/vehicle/`
- `cloud/ifex/*.yml` → `specs/cloud/`
- `docs/*-protocol.md` → `specs/protocols/`
