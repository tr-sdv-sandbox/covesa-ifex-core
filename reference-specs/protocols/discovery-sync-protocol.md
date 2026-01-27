# Discovery Sync Protocol Specification

**Version:** 1.0
**Date:** 2026-01-18

## Overview

The Discovery Sync Protocol enables efficient synchronization of IFEX service registries between vehicles and cloud. It uses content-addressed storage (SHA-256 hashes) to minimize bandwidth and avoid redundant transfers.

## Design Principles

1. **Hash as identity**: Schema content hash uniquely identifies a service definition
2. **Transfer on demand**: Full schemas only sent when cloud doesn't have them
3. **Deduplicated storage**: Identical schemas across fleet stored once
4. **Single content ID**: All discovery messages use content_id=201

## Service Registration (Vehicle-Local)

When a service starts, it registers with the local Discovery Service:

```
Service                              Discovery Service
   │                                       │
   ├─── RegisterService ───────────────────▶
   │    - service_name                     │
   │    - version                          │
   │    - endpoint_address                 │
   │    - ifex_schema (full YAML)          │
   │                                       │
   │    Discovery computes:                │
   │    schema_hash = SHA256(ifex_schema)  │
   │                                       │
   ◀─── registration_id ───────────────────┤
```

The Discovery Service:
1. Stores the full schema locally
2. Computes SHA-256 hash of the schema content
3. Makes hash available for sync queries

## Cloud Sync Protocol

### Message Types

All messages use `content_id=201`. The wire format is defined as follows:

```protobuf
// Enumerations
enum sync_event_type_t {
    FULL_SYNC = 0;             // Complete state dump (on connect/reconnect)
    SERVICE_REGISTERED = 1;    // New service registered
    SERVICE_UNREGISTERED = 2;  // Service unregistered
    SERVICE_STATUS_CHANGED = 3;// Service status or heartbeat changed
    HEARTBEAT = 4;             // Sync bridge heartbeat (no state change)
}

enum service_status_t {
    AVAILABLE = 0;
    UNAVAILABLE = 1;
    STARTING = 2;
    STOPPING = 3;
    ERROR = 4;
}

enum transport_type_t {
    GRPC = 0;
    HTTP_REST = 1;
    DBUS = 2;
    SOMEIP = 3;
    MQTT = 4;
}

// Hash-based sync messages (primary protocol)
message hash_entry_t {
    string service_name = 1;
    string version = 2;
    string schema_hash = 3;     // SHA-256 hash of IFEX schema
}

message hash_list_t {
    repeated hash_entry_t hashes = 1;
}

message schema_request_t {
    repeated string hashes = 1;  // SHA-256 hashes cloud wants
}

message schema_entry_t {
    string schema_hash = 1;
    string ifex_schema = 2;      // Full IFEX YAML
}

message schema_map_t {
    repeated schema_entry_t schemas = 1;
}

// Main envelope
message discovery_envelope_t {
    string vehicle_id = 1;
    string instance_id = 2;      // Bridge instance (for restart detection)
    hash_list_t manifest = 3;    // V2C: vehicle's hash manifest
    schema_request_t request = 4;// C2V: cloud requesting schemas
    schema_map_t schemas = 5;    // V2C: requested schemas
}
```

### Sync Flow

```
VEHICLE                                         CLOUD
   │                                              │
   │  Backend Transport queries Discovery         │
   │  for all registered service hashes           │
   │                                              │
   ├──── v2c/{vid}/201: HashList ─────────────────▶
   │     ["abc123", "def456", "ghi789"]         │
   │                                              │
   │                               ┌──────────────┤
   │                               │ For each hash:
   │                               │ - Known? Skip
   │                               │ - Unknown? Collect
   │                               └──────────────┤
   │                                              │
   │     (if any unknown hashes)                  │
   ◀──── c2v/{vid}/201: HashList ─────────────────┤
   │     ["ghi789"]                               │
   │                                              │
   │  Backend Transport queries Discovery         │
   │  for schemas matching requested hashes       │
   │                                              │
   ├──── v2c/{vid}/201: SchemaMap ────────────────▶
   │     {"ghi789": "---\nname: climate..."}      │
   │                                              │
   │                               Cloud stores   │
   │                               and parses     │
   │                               new schema     │
```

### Steady State (No Changes)

When vehicle reconnects with same software:

```
VEHICLE                                         CLOUD
   │                                              │
   ├──── v2c/{vid}/201: HashList ─────────────────▶
   │     ["abc123", "def456", "ghi789"]           │
   │                                              │
   │                               All known ─────┤
   │                               (no response)  │
   │                                              │

Total transfer: ~100 bytes
```

### Software Update Scenario

When fleet updates to new software version:

```
First vehicle with new software:
─────────────────────────────────────────────────────────
   ├──── HashList ["abc123", "NEW111"] ───────────▶
   │                                              │
   ◀──── HashList ["NEW111"] ─────────────────────┤  (request)
   │                                              │
   ├──── SchemaMap {"NEW111": "..."} ─────────────▶  (full schema)


Subsequent vehicles with same software:
─────────────────────────────────────────────────────────
   ├──── HashList ["abc123", "NEW111"] ───────────▶
   │                                              │
   │                               All known ─────┤
   │                               (no response)  │
```

## Hash Computation

The schema hash MUST be computed as:

```
hash = lowercase(hex(SHA256(ifex_schema_bytes)))
```

Where `ifex_schema_bytes` is the raw UTF-8 bytes of the IFEX YAML file, with:
- No whitespace normalization
- No YAML canonicalization
- Exact byte-for-byte content

This ensures:
- Same source file → same hash
- Different files → different hash (even if semantically equivalent)

## Backend Transport Implementation

The Backend Transport Service handles cloud sync:

```cpp
class DiscoverySyncHandler {
public:
    void PerformSync() {
        // 1. Query local Discovery for all service hashes
        auto hashes = discovery_client_.GetServiceHashes();

        // 2. Send HashList to cloud
        HashList manifest;
        for (const auto& h : hashes) {
            manifest.add_hashes(h);
        }
        Publish(CONTENT_ID_DISCOVERY, manifest);
    }

    void OnSchemaRequest(const HashList& request) {
        // Cloud is asking for specific schemas
        SchemaMap response;
        for (const auto& hash : request.hashes()) {
            auto schema = discovery_client_.GetSchemaByHash(hash);
            if (schema) {
                (*response.mutable_schemas())[hash] = *schema;
            }
        }
        Publish(CONTENT_ID_DISCOVERY, response);
    }
};
```

## Discovery Service Extensions

The Discovery Service needs to support:

```protobuf
service discovery_service {
    // Existing
    rpc register_service(register_request) returns (register_response);
    rpc query_services(query_request) returns (query_response);
    rpc get_service(get_request) returns (get_response);

    // New: hash-based queries for sync
    rpc get_service_hashes(empty) returns (HashList);
    rpc get_schemas_by_hash(HashList) returns (SchemaMap);
}
```

## Sync Triggers

Backend Transport initiates sync on:

1. **Startup**: After connecting to MQTT broker
2. **Service change**: When Discovery notifies of registration/deregistration
3. **Periodic refresh**: Optional heartbeat (e.g., every 5 minutes)
4. **On-demand**: When cloud requests via c2v

## Error Handling

| Scenario | Behavior |
|----------|----------|
| Cloud requests unknown hash | Vehicle responds with empty SchemaMap |
| Vehicle offline during request | Cloud retries on next HashList |
| Malformed schema | Cloud logs error, doesn't store |
| Hash collision (theoretical) | SHA-256 makes this negligible |

## Security Considerations

- Schemas are transmitted in plaintext (assumed TLS on MQTT)
- No authentication of schema content (trusted vehicle)
- Cloud should validate YAML syntax before storing

## Bandwidth Analysis

| Fleet Size | Services | Unique Schemas | Daily Reconnects | Daily Bandwidth |
|------------|----------|----------------|------------------|-----------------|
| 100,000 | 5/vehicle | ~10 | 100,000 | ~10 MB |
| 100,000 | 5/vehicle | ~10 | 100,000 | ~500 MB (old protocol) |

Savings: **98% bandwidth reduction** for steady-state operations.

## Compatibility

This protocol is backward compatible:

- Old vehicles (no hash support): Cloud falls back to requesting full sync
- New vehicles, old cloud: Cloud ignores HashList, no harm
- Mixed fleet: Works correctly, new vehicles benefit from optimization

## Future Extensions

1. **Schema versioning**: Track schema evolution over time
2. **Diff-based updates**: Send only changed portions of schema
3. **Compression**: Compress SchemaMap payloads for large schemas
4. **Signing**: Cryptographic signatures on schemas for integrity
