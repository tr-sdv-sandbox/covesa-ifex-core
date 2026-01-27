// AUTO-GENERATED - DO NOT EDIT
// Generated from: reference-specs/scheduler/cloud/cloud-scheduler-service.ifex.yml
// Regenerate with: ./generate_proto.sh

#pragma once

namespace ifex::schema {

inline constexpr const char* cloud_scheduler_service = R"IFEX(
# AUTO-GENERATED - DO NOT EDIT
# Flattened from: /home/saka/BALI/tvep-workspace/components/covesa-ifex-core/reference-specs/scheduler/cloud/cloud-scheduler-service.ifex.yml
# Regenerate with: ./generate_proto.sh
---
name: cloud_scheduler_service
major_version: 1
minor_version: 0
description: 'Cloud-side fleet scheduler service for managing scheduled jobs on vehicles.
  Provides job creation, management, and fleet-wide scheduling operations. Jobs are
  synced to vehicles via the scheduler sync protocol (content_id=202).

  Aligned with scheduler-sync-protocol-v2.md specification.

  '
namespaces:
- name: scheduler_types
  description: Types from included scheduler_types
  enumerations:
  - name: job_authority_t
    datatype: uint8
    description: 'Who is authoritative for conflicts on this job. Set at creation
      time, immutable thereafter. Used by sync protocol to resolve concurrent modifications.

      '
    options:
    - name: AUTHORITY_CLOUD
      value: 0
      description: Cloud wins conflicts (job created by cloud)
    - name: AUTHORITY_VEHICLE
      value: 1
      description: Vehicle wins conflicts (job created on vehicle/phone)
  - name: job_status_t
    datatype: uint8
    description: 'Job execution status. Vehicle-authoritative - only vehicle updates
      this field based on actual execution state.

      '
    options:
    - name: JOB_STATUS_PENDING
      value: 0
      description: Waiting to execute
    - name: JOB_STATUS_RUNNING
      value: 1
      description: Currently executing
    - name: JOB_STATUS_COMPLETED
      value: 2
      description: Finished successfully
    - name: JOB_STATUS_FAILED
      value: 3
      description: Execution failed
    - name: JOB_STATUS_CANCELLED
      value: 4
      description: Cancelled by user/system
  - name: sync_state_t
    datatype: uint8
    description: 'Synchronization state between local and remote. Derived locally
      by comparing version vectors, NOT transmitted. Used for UI display and debugging.

      '
    options:
    - name: SYNC_PENDING
      value: 0
      description: My version differs from last confirmed remote version
    - name: SYNC_SYNCED
      value: 1
      description: My version matches last confirmed remote version
  - name: wake_policy_t
    datatype: uint8
    description: 'Vehicle wake behavior for scheduled jobs. Determines whether job
      can wake vehicle from sleep via RTC.

      '
    options:
    - name: WAKE_NO_WAKE
      value: 0
      description: Only run if vehicle already awake
    - name: WAKE_REQUIRED
      value: 1
      description: Wake vehicle via RTC timer to run job
  - name: sleep_policy_t
    datatype: uint8
    description: 'Vehicle sleep behavior during job execution. Controls whether vehicle
      can sleep while job is running.

      '
    options:
    - name: SLEEP_NORMAL
      value: 0
      description: Normal sleep behavior after job completes
    - name: SLEEP_INHIBIT
      value: 1
      description: Prevent sleep until job execution finishes
  structs:
  - name: job_version_t
    description: 'Two-component version vector for conflict detection. Each component
      is incremented by its respective side on any change.

      '
    members:
    - name: cloud_seq
      datatype: uint64
      description: Incremented by cloud on any change
    - name: vehicle_seq
      datatype: uint64
      description: Incremented by vehicle on any change
  - name: job_t
    description: 'Complete job record with version tracking. This is the canonical
      job structure used by both vehicle and cloud. Aligned with scheduler-sync-protocol-v2.md
      specification.

      '
    members:
    - name: job_id
      datatype: string
      description: 'Globally unique ID with source namespace. Format: <source>-<uuid>
        where source is "cloud", "veh-<vin>", or "phone"

        '
    - name: authority
      datatype: job_authority_t
      description: Who wins conflicts (immutable after creation)
    - name: cloud_seq
      datatype: uint64
      description: Cloud sequence number (version vector component)
    - name: vehicle_seq
      datatype: uint64
      description: Vehicle sequence number (version vector component)
    - name: deleted
      datatype: boolean
      description: Soft delete tombstone flag
      mandatory: false
    - name: title
      datatype: string
      description: Human-readable job title
    - name: service
      datatype: string
      description: Target service name
    - name: method
      datatype: string
      description: Target method name
    - name: parameters_json
      datatype: string
      description: JSON-encoded method parameters
      mandatory: false
    - name: scheduled_time_ms
      datatype: uint64
      description: When to run (epoch milliseconds UTC)
    - name: recurrence_rule
      datatype: string
      description: iCal RRULE for recurring jobs
      mandatory: false
    - name: end_time_ms
      datatype: uint64
      description: Stop recurring after this (epoch ms, 0 = forever)
      mandatory: false
    - name: paused
      datatype: boolean
      description: User intent - job should not be scheduled until resumed
      mandatory: false
    - name: wake_policy
      datatype: wake_policy_t
      description: Whether to wake vehicle for this job
      mandatory: false
    - name: sleep_policy
      datatype: sleep_policy_t
      description: Sleep behavior during execution
      mandatory: false
    - name: wake_lead_time_s
      datatype: uint32
      description: Seconds before scheduled_time to wake vehicle
      mandatory: false
    - name: status
      datatype: job_status_t
      description: Current execution status
    - name: next_run_time_ms
      datatype: uint64
      description: Next scheduled run time (epoch ms)
      mandatory: false
    - name: last_executed_ms
      datatype: uint64
      description: Last execution timestamp (epoch ms)
      mandatory: false
    - name: vehicle_id
      datatype: string
      description: 'Target vehicle ID. Assigned by cloud, not synced to vehicle. Vehicle
        scheduler ignores this field.

        '
      mandatory: false
    - name: created_at_ms
      datatype: uint64
      description: Creation timestamp (epoch ms)
    - name: updated_at_ms
      datatype: uint64
      description: Last update timestamp (epoch ms)
    - name: created_by
      datatype: string
      description: User/system that created job
      mandatory: false
    - name: sync_state
      datatype: sync_state_t
      description: 'Synchronization state derived from comparing local version with
        last confirmed remote version. Populated by scheduler service for UI display.
        NOT included in sync messages.

        '
      mandatory: false
  - name: execution_record_t
    description: 'Immutable record of a job execution. Executions are append-only
      facts - they never conflict. Used by sync protocol for execution history.

      '
    members:
    - name: execution_id
      datatype: string
      description: Globally unique execution identifier
    - name: job_id
      datatype: string
      description: Job that was executed
    - name: executed_at_ms
      datatype: uint64
      description: When execution started (epoch ms)
    - name: duration_ms
      datatype: uint32
      description: Execution duration in milliseconds
    - name: status
      datatype: job_status_t
      description: Result - JOB_STATUS_COMPLETED or JOB_STATUS_FAILED
    - name: result_json
      datatype: string
      description: JSON result (for completed jobs)
      mandatory: false
    - name: error_message
      datatype: string
      description: Error message (for failed jobs)
      mandatory: false
- name: scheduler
  description: Cloud scheduler operations for fleet job management
  structs:
  - name: create_job_request_t
    description: Request to create a new scheduled job
    members:
    - name: vehicle_id
      datatype: string
    - name: title
      datatype: string
      description: Human-readable title
    - name: service
      datatype: string
      description: Target service name
    - name: method
      datatype: string
      description: Method to invoke
    - name: parameters_json
      datatype: string
      mandatory: false
    - name: scheduled_time_ms
      datatype: uint64
      description: When to first run (epoch milliseconds UTC)
    - name: recurrence_rule
      datatype: string
      description: iCal RRULE for recurring jobs
      mandatory: false
    - name: end_time_ms
      datatype: uint64
      description: When to stop recurring (epoch milliseconds UTC)
      mandatory: false
    - name: created_by
      datatype: string
      mandatory: false
    - name: paused
      datatype: boolean
      description: Create in paused state
      mandatory: false
    - name: wake_policy
      datatype: scheduler_types.wake_policy_t
      mandatory: false
    - name: sleep_policy
      datatype: scheduler_types.sleep_policy_t
      mandatory: false
    - name: wake_lead_time_s
      datatype: uint32
      mandatory: false
  - name: create_job_response_t
    description: Result of job creation
    members:
    - name: success
      datatype: boolean
    - name: job_id
      datatype: string
      description: Cloud-generated job ID (cloud-<uuid>)
      mandatory: false
    - name: error_message
      datatype: string
      mandatory: false
  - name: update_job_request_t
    description: Request to update a job (partial update, empty/zero = no change)
    members:
    - name: vehicle_id
      datatype: string
    - name: job_id
      datatype: string
    - name: title
      datatype: string
      description: Empty = no change
      mandatory: false
    - name: scheduled_time_ms
      datatype: uint64
      description: 0 = no change (epoch ms UTC)
      mandatory: false
    - name: recurrence_rule
      datatype: string
      mandatory: false
    - name: parameters_json
      datatype: string
      mandatory: false
    - name: end_time_ms
      datatype: uint64
      description: 0 = no change (epoch ms UTC)
      mandatory: false
    - name: has_paused
      datatype: boolean
      mandatory: false
    - name: paused
      datatype: boolean
      mandatory: false
    - name: has_wake_policy
      datatype: boolean
      mandatory: false
    - name: wake_policy
      datatype: scheduler_types.wake_policy_t
      mandatory: false
    - name: has_sleep_policy
      datatype: boolean
      mandatory: false
    - name: sleep_policy
      datatype: scheduler_types.sleep_policy_t
      mandatory: false
    - name: has_wake_lead_time
      datatype: boolean
      mandatory: false
    - name: wake_lead_time_s
      datatype: uint32
      mandatory: false
  - name: simple_response_t
    description: Simple success/error response
    members:
    - name: success
      datatype: boolean
    - name: error_message
      datatype: string
      mandatory: false
  - name: get_job_response_t
    description: Response for get job request
    members:
    - name: found
      datatype: boolean
    - name: job
      datatype: scheduler_types.job_t
      mandatory: false
  - name: list_jobs_filter_t
    description: Filters for listing jobs
    members:
    - name: vehicle_id_filter
      datatype: string
      mandatory: false
    - name: fleet_id_filter
      datatype: string
      mandatory: false
    - name: region_filter
      datatype: string
      mandatory: false
    - name: service_filter
      datatype: string
      mandatory: false
    - name: status_filter
      datatype: scheduler_types.job_status_t
      description: 0 = all statuses
      mandatory: false
    - name: created_by_filter
      datatype: string
      mandatory: false
    - name: start_time_ms
      datatype: uint64
      description: If set, only jobs with scheduled_time >= this value
      mandatory: false
    - name: end_time_ms
      datatype: uint64
      description: If set, only jobs with scheduled_time <= this value
      mandatory: false
    - name: recurring_only
      datatype: boolean
      mandatory: false
    - name: paused_only
      datatype: boolean
      mandatory: false
    - name: include_deleted
      datatype: boolean
      description: Include tombstones
      mandatory: false
    - name: page_size
      datatype: int32
      mandatory: false
    - name: page_token
      datatype: string
      mandatory: false
  - name: list_jobs_response_t
    description: List of jobs
    members:
    - name: jobs
      datatype: scheduler_types.job_t[]
    - name: next_page_token
      datatype: string
      mandatory: false
    - name: total_count
      datatype: int32
  - name: list_jobs_hash_response_t
    description: Hash of job state for change detection
    members:
    - name: state_hash
      datatype: uint64
      description: xxHash64 of filtered job state (IDs, versions, statuses)
    - name: job_count
      datatype: int32
      description: Number of jobs matching the filter
  - name: list_executions_filter_t
    description: 'Filter criteria for retrieving executions. All filters are optional
      - empty filter returns all executions.

      '
    members:
    - name: vehicle_id
      datatype: string
      description: If set, only executions from this vehicle
      mandatory: false
    - name: job_id
      datatype: string
      description: If set, only executions for this job
      mandatory: false
    - name: fleet_id
      datatype: string
      description: If set, only executions from vehicles in this fleet
      mandatory: false
    - name: start_time_ms
      datatype: uint64
      description: If set, only executions with executed_at >= this value
      mandatory: false
    - name: end_time_ms
      datatype: uint64
      description: If set, only executions with executed_at <= this value
      mandatory: false
    - name: status_filter
      datatype: scheduler_types.job_status_t
      description: If set (non-zero), only executions with this status
      mandatory: false
    - name: limit
      datatype: int32
      description: Maximum number of executions to return (default 100)
      mandatory: false
    - name: offset
      datatype: int32
      description: Number of executions to skip (for pagination)
      mandatory: false
  - name: list_executions_response_t
    description: List of execution records
    members:
    - name: executions
      datatype: scheduler_types.execution_record_t[]
    - name: total_count
      datatype: int32
      description: Total matching executions (for pagination)
  - name: list_executions_hash_response_t
    description: Hash of execution state for change detection
    members:
    - name: state_hash
      datatype: uint64
      description: xxHash64 of the filtered execution state
    - name: execution_count
      datatype: int32
      description: Number of executions matching the filter
  methods:
  - name: create_job
    description: Create a new scheduled job on a vehicle
    input:
    - name: request
      datatype: create_job_request_t
      mandatory: true
    output:
    - name: result
      datatype: create_job_response_t
  - name: update_job
    description: Update an existing job (partial update)
    input:
    - name: request
      datatype: update_job_request_t
      mandatory: true
    output:
    - name: result
      datatype: simple_response_t
  - name: delete_job
    description: Delete a job
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    - name: job_id
      datatype: string
      mandatory: true
    output:
    - name: result
      datatype: simple_response_t
  - name: pause_job
    description: Pause a job (stop scheduling)
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    - name: job_id
      datatype: string
      mandatory: true
    output:
    - name: result
      datatype: simple_response_t
  - name: resume_job
    description: Resume a paused job
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    - name: job_id
      datatype: string
      mandatory: true
    output:
    - name: result
      datatype: simple_response_t
  - name: trigger_job
    description: Trigger immediate job execution
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    - name: job_id
      datatype: string
      mandatory: true
    output:
    - name: result
      datatype: simple_response_t
  - name: get_job
    description: Get detailed job information
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    - name: job_id
      datatype: string
      mandatory: true
    output:
    - name: result
      datatype: get_job_response_t
  - name: list_jobs
    description: List jobs with optional filtering
    input:
    - name: filter
      datatype: list_jobs_filter_t
      mandatory: false
    output:
    - name: result
      datatype: list_jobs_response_t
  - name: list_jobs_hash
    description: 'Get a hash of the job state matching the filter criteria. Used for
      efficient change detection - poll this endpoint and only fetch full job list
      when hash changes. Uses same filter as list_jobs.

      '
    input:
    - name: filter
      datatype: list_jobs_filter_t
      description: Filter criteria (same as list_jobs)
      mandatory: false
    output:
    - name: result
      datatype: list_jobs_hash_response_t
  - name: list_executions
    description: 'Retrieve execution history with optional filters. Returns immutable
      records of past job executions.

      '
    input:
    - name: filter
      datatype: list_executions_filter_t
      description: Filter criteria (empty for all executions)
      mandatory: false
    output:
    - name: result
      datatype: list_executions_response_t
  - name: list_executions_hash
    description: 'Get a hash of the execution state matching the filter criteria.
      Used for efficient change detection - poll this endpoint and only fetch full
      execution list when hash changes.

      '
    input:
    - name: filter
      datatype: list_executions_filter_t
      description: Filter criteria (same as list_executions)
      mandatory: false
    output:
    - name: result
      datatype: list_executions_hash_response_t
  - name: healthy
    description: Check if scheduler service is ready
    output:
    - name: is_healthy
      datatype: boolean
- name: internal
  description: 'Internal API for cloud-scheduler-sync-bridge. Generic job CRUD with
    version vectors - no sync protocol types. The scheduler maintains cloud_checksum
    on job changes.

    '
  structs:
  - name: vehicle_sync_state_t
    description: Per-vehicle sync state (checksums for quiescence detection)
    members:
    - name: vehicle_id
      datatype: string
    - name: cloud_checksum
      datatype: uint64
      description: 'Checksum of cloud''s job state for this vehicle. MAINTAINED BY
        SCHEDULER: recomputed on every job create/update/delete.

        '
    - name: last_seen_v2c_checksum
      datatype: uint64
      description: Last V2C checksum received from vehicle (updated by sync bridge)
    - name: last_sync_timestamp_ms
      datatype: uint64
  methods:
  - name: get_jobs_for_vehicle
    description: 'Get all jobs assigned to a vehicle. Used by sync bridge to build
      C2V messages.

      '
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    - name: include_deleted
      datatype: boolean
      description: Include soft-deleted jobs (tombstones)
      mandatory: false
    output:
    - name: jobs
      datatype: scheduler_types.job_t[]
  - name: upsert_job
    description: 'Create or update a job with version vector fields. Used by sync
      bridge when processing V2C messages. Scheduler stores data and recomputes cloud_checksum.

      '
    input:
    - name: job
      datatype: scheduler_types.job_t
      mandatory: true
    output:
    - name: success
      datatype: boolean
    - name: updated_job
      datatype: scheduler_types.job_t
  - name: record_execution
    description: 'Record a job execution (append-only, idempotent by execution_id).
      Used by sync bridge when processing V2C execution reports.

      '
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    - name: job_id
      datatype: string
      mandatory: true
    - name: execution
      datatype: scheduler_types.execution_record_t
      mandatory: true
    output:
    - name: success
      datatype: boolean
    - name: is_duplicate
      datatype: boolean
      description: True if execution_id already existed
  - name: get_vehicle_sync_state
    description: 'Get sync state for a vehicle (checksums for quiescence detection).

      '
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    output:
    - name: state
      datatype: vehicle_sync_state_t
    - name: found
      datatype: boolean
  - name: update_vehicle_sync_state
    description: 'Update sync state for a vehicle after processing V2C message. Only
      updates last_seen_v2c_checksum and timestamp. cloud_checksum is maintained internally
      by scheduler.

      '
    input:
    - name: vehicle_id
      datatype: string
      mandatory: true
    - name: last_seen_v2c_checksum
      datatype: uint64
      mandatory: true
    output:
    - name: success
      datatype: boolean
  - name: get_pending_syncs
    description: 'Get vehicles that have pending changes to sync. Returns vehicles
      where cloud_checksum != last_seen_v2c_checksum. Used by sync bridge to know
      which vehicles need C2V messages.

      '
    input:
    - name: limit
      datatype: int32
      description: Max vehicles to return (0 = no limit)
      mandatory: false
    output:
    - name: pending_vehicles
      datatype: vehicle_sync_state_t[]
)IFEX";

}  // namespace ifex::schema
