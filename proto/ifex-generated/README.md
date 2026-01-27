# IFEX-Generated Proto Files

**DO NOT EDIT THESE FILES MANUALLY**

These proto files are automatically generated from IFEX YAML specifications.

## Directory Structure

```
proto/ifex-generated/
├── vehicle/           # Vehicle-side service protos
│   ├── backend-transport-service.proto
│   ├── discovery-service.proto
│   ├── dispatcher-service.proto
│   └── scheduler-service.proto
│
├── cloud/             # Cloud-side service protos
│   ├── cloud-backend-transport-service.proto
│   ├── cloud-discovery-service.proto
│   ├── cloud-dispatcher-service.proto
│   └── cloud-scheduler-service.proto
│
└── test-services/     # Test service protos
    └── ...
```

## Regenerating

```bash
./generate_proto.sh
```

## Source Files

The IFEX YAML source files are located in:
- `reference-specs/vehicle/` - Vehicle service definitions
- `reference-specs/cloud/` - Cloud service definitions
- `test-services/*/` - Test service definitions

## If You Need to Change a Proto

1. Edit the corresponding `.ifex.yml` file in `reference-specs/`
2. Run `./generate_proto.sh`
3. Rebuild: `cmake --build build`

Do NOT edit these `.proto` files directly - your changes will be overwritten.
