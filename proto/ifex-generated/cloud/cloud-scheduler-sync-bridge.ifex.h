// AUTO-GENERATED - DO NOT EDIT
// Generated from: reference-specs/cloud/cloud-scheduler-sync-bridge.ifex.yml
// Regenerate with: ./generate_proto.sh

#pragma once

namespace ifex::schema {

inline constexpr const char* cloud_scheduler_sync_bridge = R"IFEX(
# AUTO-GENERATED - DO NOT EDIT
# Flattened from: /home/saka/BALI/tvep-workspace/components/covesa-ifex-core/reference-specs/cloud/cloud-scheduler-sync-bridge.ifex.yml
# Regenerate with: ./generate_proto.sh
---
name: cloud_scheduler_sync_bridge
major_version: 1
minor_version: 0
description: 'Cloud-side scheduler sync bridge. Handles bidirectional sync between
  cloud scheduler and vehicles. Uses gRPC interfaces only - agnostic to scheduler/transport
  implementations.

  The sync bridge: - Subscribes to V2C messages from vehicles - Processes sync messages
  using version vector comparison - Calls scheduler internal API for job CRUD - Sends
  C2V sync messages back to vehicles - Detects quiescence via checksum comparison

  '
namespaces:
- name: bridge
  description: Sync bridge control and monitoring
  enumerations:
  - name: bridge_status_t
    datatype: uint8
    description: Current bridge status
    options:
    - name: STATUS_STARTING
      value: 0
    - name: STATUS_RUNNING
      value: 1
    - name: STATUS_STOPPING
      value: 2
    - name: STATUS_STOPPED
      value: 3
    - name: STATUS_ERROR
      value: 4
  structs:
  - name: bridge_stats_t
    description: Bridge statistics
    members:
    - name: v2c_messages_received
      datatype: uint64
      description: Total V2C messages received from vehicles
    - name: v2c_messages_processed
      datatype: uint64
      description: V2C messages successfully processed
    - name: c2v_messages_sent
      datatype: uint64
      description: C2V messages sent to vehicles
    - name: jobs_upserted
      datatype: uint64
      description: Jobs created/updated via upsert
    - name: executions_recorded
      datatype: uint64
      description: Execution records stored
    - name: quiescent_skipped
      datatype: uint64
      description: Syncs skipped due to quiescence (checksums match)
    - name: conflicts_resolved
      datatype: uint64
      description: Version vector conflicts resolved
    - name: errors
      datatype: uint64
      description: Processing errors encountered
    - name: vehicles_seen
      datatype: uint32
      description: Unique vehicles that have synced
    - name: uptime_ms
      datatype: uint64
      description: Bridge uptime in milliseconds
  - name: vehicle_sync_info_t
    description: Sync info for a specific vehicle
    members:
    - name: vehicle_id
      datatype: string
    - name: last_v2c_timestamp_ms
      datatype: uint64
      description: Last V2C message received
    - name: last_c2v_timestamp_ms
      datatype: uint64
      description: Last C2V message sent
    - name: cloud_checksum
      datatype: uint64
      description: Current cloud state checksum
    - name: last_seen_v2c_checksum
      datatype: uint64
      description: Last V2C checksum from vehicle
    - name: is_quiescent
      datatype: boolean
      description: True if checksums match (no sync needed)
    - name: job_count
      datatype: uint32
      description: Number of jobs for this vehicle
  - name: bridge_health_t
    description: Bridge health status
    members:
    - name: status
      datatype: bridge_status_t
    - name: scheduler_connected
      datatype: boolean
    - name: transport_connected
      datatype: boolean
    - name: last_error
      datatype: string
      mandatory: false
    - name: last_error_timestamp_ms
      datatype: uint64
      mandatory: false
  methods:
  - name: get_stats
    description: Get bridge statistics
    output:
    - name: stats
      datatype: bridge_stats_t
  - name: get_health
    description: Get bridge health status
    output:
    - name: health
      datatype: bridge_health_t
  - name: get_vehicle_sync_info
    description: Get sync info for a specific vehicle
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    output:
    - name: info
      datatype: vehicle_sync_info_t
    - name: found
      datatype: boolean
  - name: force_sync
    description: Force sync for a specific vehicle (bypasses quiescence)
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    output:
    - name: success
      datatype: boolean
    - name: error_message
      datatype: string
      mandatory: false
  - name: healthy
    description: Simple health check
    output:
    - name: is_healthy
      datatype: boolean
)IFEX";

}  // namespace ifex::schema
