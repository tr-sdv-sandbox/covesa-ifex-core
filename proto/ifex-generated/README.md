# IFEX-Generated Files

**DO NOT EDIT THESE FILES MANUALLY**

These files are automatically generated from IFEX YAML specifications.

## What's Generated

The `generate_proto.sh` script produces two types of files:

1. **Proto files** (`*.proto`) - For gRPC code generation
2. **C++ headers** (`*.ifex.h`) - Embedded IFEX schemas as raw strings

## Directory Structure

```
proto/ifex-generated/
├── common/            # Shared types
│   ├── scheduler-types.proto
│   └── scheduler-types.ifex.h
│
├── vehicle/           # Vehicle-side services
│   ├── scheduler-service.proto
│   ├── scheduler-service.ifex.h    # Embedded flattened IFEX
│   ├── dispatcher-service.proto
│   ├── dispatcher-service.ifex.h
│   └── ...
│
├── cloud/             # Cloud-side services
│   ├── cloud-scheduler-service.proto
│   ├── cloud-scheduler-service.ifex.h
│   └── ...
│
└── test-services/     # Test services
    ├── echo_service.proto
    ├── echo_service.ifex.h
    └── ...
```

## Using Embedded Schemas

Services include the generated header to register with Discovery:

```cpp
#include "scheduler-service.ifex.h"

// Register with service discovery
discovery_client.register_service(ifex::schema::scheduler_service, port);
```

The `ifex::schema::*` constants contain the complete flattened IFEX YAML as compile-time strings. No runtime file loading needed.

## Regenerating

```bash
./generate_proto.sh
```

## Source Files

The IFEX YAML source files are located in:
- `reference-specs/vehicle/` - Vehicle service definitions
- `reference-specs/cloud/` - Cloud service definitions
- `reference-specs/common/` - Shared type definitions
- `test-services/*/` - Test service definitions

## If You Need to Change a Proto

1. Edit the corresponding `.ifex.yml` file in `reference-specs/` or `test-services/`
2. Run `./generate_proto.sh`
3. Rebuild: `./build.sh`

Do NOT edit these files directly - your changes will be overwritten.
