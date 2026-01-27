// AUTO-GENERATED - DO NOT EDIT
// Generated from: reference-specs/discovery/cloud/cloud-discovery-service.ifex.yml
// Regenerate with: ./generate_proto.sh

#pragma once

namespace ifex::schema {

inline constexpr const char* cloud_discovery_service = R"IFEX(
# AUTO-GENERATED - DO NOT EDIT
# Flattened from: /home/saka/BALI/tvep-workspace/components/covesa-ifex-core/reference-specs/discovery/cloud/cloud-discovery-service.ifex.yml
# Regenerate with: ./generate_proto.sh
---
name: cloud_discovery_service
major_version: 1
minor_version: 0
description: 'Cloud-side service for querying vehicle service registries. Aggregates
  Discovery state from all vehicles via sync protocol (content_id=201). Provides fleet-wide
  service discovery and capability queries.

  '
namespaces:
- name: discovery
  description: Fleet-wide service discovery operations
  enumerations:
  - name: service_status_t
    datatype: uint8
    description: Service availability status
    options:
    - name: AVAILABLE
      value: 0
    - name: UNAVAILABLE
      value: 1
    - name: STARTING
      value: 2
    - name: STOPPING
      value: 3
    - name: ERROR
      value: 4
  - name: transport_type_t
    datatype: uint8
    description: Service transport protocol
    options:
    - name: GRPC
      value: 0
    - name: HTTP_REST
      value: 1
    - name: DBUS
      value: 2
    - name: SOMEIP
      value: 3
    - name: MQTT
      value: 4
  - name: sync_status_t
    datatype: uint8
    description: Vehicle sync status
    options:
    - name: UNKNOWN
      value: 0
      description: Vehicle has never synced
    - name: SYNCED
      value: 1
      description: Vehicle state is current
    - name: STALE
      value: 2
      description: Vehicle offline, state may be outdated
    - name: SYNCING
      value: 3
      description: Sync in progress
  structs:
  - name: method_info_t
    description: Service method metadata
    members:
    - name: name
      datatype: string
    - name: description
      datatype: string
      mandatory: false
  - name: namespace_info_t
    description: Service namespace with methods
    members:
    - name: name
      datatype: string
    - name: methods
      datatype: method_info_t[]
  - name: service_endpoint_t
    description: Service network endpoint
    members:
    - name: address
      datatype: string
      description: Host:port or URL
    - name: transport
      datatype: transport_type_t
  - name: vehicle_service_t
    description: Service registered on a specific vehicle
    members:
    - name: vehicle_id
      datatype: string
    - name: registration_id
      datatype: string
      description: Unique registration handle on vehicle
    - name: name
      datatype: string
    - name: version
      datatype: string
    - name: description
      datatype: string
      mandatory: false
    - name: endpoint
      datatype: service_endpoint_t
    - name: status
      datatype: service_status_t
    - name: last_heartbeat_ms
      datatype: int64
    - name: namespaces
      datatype: namespace_info_t[]
    - name: schema_hash
      datatype: string
      description: SHA-256 hash of IFEX schema
  - name: vehicle_sync_info_t
    description: Vehicle discovery sync state
    members:
    - name: vehicle_id
      datatype: string
    - name: sync_status
      datatype: sync_status_t
    - name: last_sync_ms
      datatype: int64
    - name: service_count
      datatype: uint32
    - name: state_checksum
      datatype: uint32
  - name: service_schema_t
    description: Full IFEX schema for a service
    members:
    - name: schema_hash
      datatype: string
    - name: service_name
      datatype: string
    - name: version
      datatype: string
    - name: ifex_yaml
      datatype: string
      description: Full IFEX YAML content
    - name: first_seen_ms
      datatype: int64
    - name: vehicle_count
      datatype: uint32
      description: Number of vehicles with this schema
  - name: service_filter_t
    description: Filter criteria for service queries
    members:
    - name: vehicle_id
      datatype: string
      mandatory: false
    - name: fleet_id
      datatype: string
      mandatory: false
    - name: service_name
      datatype: string
      mandatory: false
    - name: has_method
      datatype: string
      description: Filter by method name
      mandatory: false
    - name: status
      datatype: service_status_t
      mandatory: false
    - name: include_offline_vehicles
      datatype: boolean
      default: false
  - name: fleet_capability_t
    description: Aggregated capability across fleet
    members:
    - name: service_name
      datatype: string
    - name: version
      datatype: string
    - name: vehicle_count
      datatype: uint32
      description: Vehicles with this service
    - name: available_count
      datatype: uint32
      description: Vehicles where service is AVAILABLE
    - name: methods
      datatype: string[]
      description: Union of all methods across versions
  methods:
  - name: get_vehicle_services
    description: Get all services registered on a specific vehicle
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    output:
    - name: services
      datatype: vehicle_service_t[]
    - name: sync_info
      datatype: vehicle_sync_info_t
  - name: find_services
    description: Search for services across fleet
    input:
    - name: filter
      datatype: service_filter_t
      mandatory: false
    output:
    - name: services
      datatype: vehicle_service_t[]
    - name: total_count
      datatype: uint32
  - name: get_fleet_capabilities
    description: Get aggregated service capabilities across fleet
    input:
    - name: fleet_id
      datatype: string
      mandatory: false
    output:
    - name: capabilities
      datatype: fleet_capability_t[]
  - name: get_schema
    description: Get full IFEX schema by hash
    input:
    - name: schema_hash
      datatype: string
      mandatory: true
    output:
    - name: found
      datatype: boolean
    - name: schema
      datatype: service_schema_t
      mandatory: false
  - name: list_schemas
    description: List all known service schemas
    input:
    - name: service_name_filter
      datatype: string
      mandatory: false
    output:
    - name: schemas
      datatype: service_schema_t[]
  - name: get_vehicle_sync_status
    description: Get sync status for a vehicle
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    output:
    - name: info
      datatype: vehicle_sync_info_t
  - name: list_vehicles
    description: List vehicles with their sync status
    input:
    - name: fleet_id
      datatype: string
      mandatory: false
    - name: sync_status_filter
      datatype: sync_status_t
      mandatory: false
    output:
    - name: vehicles
      datatype: vehicle_sync_info_t[]
  - name: healthy
    description: Check if discovery service is ready
    output:
    - name: is_healthy
      datatype: boolean
)IFEX";

}  // namespace ifex::schema
