# IFEX Specifications

This directory contains the authoritative specifications for IFEX services and protocols.

## Directory Structure

```
reference-specs/
├── scheduler/                    # Scheduler service specs
│   ├── vehicle/
│   │   └── scheduler-service.ifex.yml
│   ├── cloud/
│   │   ├── cloud-scheduler-service.ifex.yml
│   │   └── cloud-scheduler-sync-bridge.ifex.yml
│   └── common/
│       └── scheduler-types.ifex.yml   # Shared types
│
├── discovery/                    # Discovery service specs
│   ├── vehicle/
│   │   └── discovery-service.ifex.yml
│   └── cloud/
│       └── cloud-discovery-service.ifex.yml
│
├── dispatcher/                   # Dispatcher service specs
│   ├── vehicle/
│   │   └── dispatcher-service.ifex.yml
│   └── cloud/
│       └── cloud-dispatcher-service.ifex.yml
│
├── backend-transport/            # Backend transport specs
│   ├── vehicle/
│   │   └── backend-transport-service.ifex.yml
│   └── cloud/
│       └── cloud-backend-transport-service.ifex.yml
│
└── protocols/                    # Internal transfer protocols (source of truth)
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

The specs define **what** services do. Reference implementations in `reference-services/` show **how** to implement them.

```
reference-specs/scheduler/vehicle/scheduler-service.ifex.yml  →  reference-services/scheduler/vehicle/service/
reference-specs/scheduler/cloud/cloud-scheduler-service.ifex.yml  →  reference-services/scheduler/cloud/service/
reference-specs/protocols/scheduler-sync-v2.md  →  Used by both sides
```
