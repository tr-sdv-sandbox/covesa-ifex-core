# API Proto Definitions

**These files are authoritative - edit directly**

This directory contains hand-written proto definitions for external-facing
gRPC APIs that are not generated from IFEX.

## Contents

| File | Purpose |
|------|---------|
| `cloud-scheduler-service.proto` | Cloud scheduler gRPC API (fleet job management) |

## When to Add Here

Add a proto here when:
- Defining a gRPC service API
- The API is external-facing or has specific requirements not expressible in IFEX
- You need fine-grained control over proto features (oneof, maps, extensions)

## Relationship to IFEX

IFEX is preferred for vehicle service interfaces. Use this directory when:
- The service is cloud-only (not on vehicle)
- You need proto features IFEX doesn't support
- The API is internal tooling, not a vehicle capability
