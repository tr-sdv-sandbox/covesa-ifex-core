# Internal Protocol Definitions

**These files are authoritative - edit directly**

This directory contains hand-written proto definitions for internal
communication protocols between IFEX components.

## Contents

| File | Purpose |
|------|---------|
| `scheduler-sync-v2.proto` | Bidirectional sync protocol between vehicle and cloud scheduler |

## Design Principles

These protos define **internal wire protocols** that are:
- Not exposed to external consumers
- Controlled by us on both ends (onboard and offboard)
- Versioned via proto field numbers (not IFEX)

## When to Add Here

Add a proto here when:
- Defining communication between internal components
- The protocol is not a service interface (no gRPC service)
- Both ends of the protocol are under our control

For service interfaces exposed to external consumers, use IFEX instead.
