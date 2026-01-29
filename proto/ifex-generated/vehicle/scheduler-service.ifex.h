// AUTO-GENERATED - DO NOT EDIT
// Generated from: reference-specs/scheduler/vehicle/scheduler-service.ifex.yml
// Regenerate with: ./generate_proto.sh

#pragma once

namespace ifex::schema {

inline constexpr const char* scheduler_service = R"IFEX(
# AUTO-GENERATED - DO NOT EDIT
# Flattened from: /home/saka/BALI/tvep-workspace/components/covesa-ifex-core/reference-specs/scheduler/vehicle/scheduler-service.ifex.yml
# Regenerate with: ./generate_proto.sh
---
name: ifex_scheduler
major_version: 2
minor_version: 1
description: Calendar-style scheduler for IFEX-defined services
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
    - name: local_version
      datatype: job_version_t
      description: 'My current version of this job. Contains cloud_seq and vehicle_seq.
        Transmitted in sync messages.

        '
    - name: remote_version
      datatype: job_version_t
      description: 'Last known remote version (what I believe remote has confirmed).
        Local tracking only - used to compute is_dirty(). NOT transmitted in sync
        messages.

        '
      mandatory: false
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
- name: core
  description: Core scheduling functionality
  structs:
  - name: job_filter_t
    description: 'Filter criteria for retrieving jobs. By default returns active (non-deleted)
      jobs. Set include_deleted=true to include tombstones (for sync protocol).

      '
    members:
    - name: start_time_ms
      datatype: uint64
      description: If set, only jobs with scheduled_time >= this value
      mandatory: false
    - name: end_time_ms
      datatype: uint64
      description: If set, only jobs with scheduled_time <= this value
      mandatory: false
    - name: service
      datatype: string
      description: If set, only jobs targeting this service
      mandatory: false
    - name: status
      datatype: scheduler_types.job_status_t
      description: If set (with has_status_filter=true), only jobs with this status
      mandatory: false
    - name: has_status_filter
      datatype: boolean
      description: Whether to apply status filter (needed because status enum has
        no "unset" value)
      mandatory: false
      default: false
    - name: include_deleted
      datatype: boolean
      description: If true, include soft-deleted jobs (tombstones). Default false.
      mandatory: false
      default: false
  - name: job_create_t
    description: Information needed to create a new job
    members:
    - name: job_id
      datatype: string
      description: Optional job ID (if empty, scheduler generates one)
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
    - name: scheduled_time_ms
      datatype: uint64
      description: Scheduled execution time (epoch milliseconds UTC)
    - name: recurrence_rule
      datatype: string
      description: Cron expression (e.g., "0 8 * * MON-FRI") or RRULE
      mandatory: false
    - name: end_time_ms
      datatype: uint64
      description: End time for recurring jobs (epoch milliseconds UTC)
      mandatory: false
    - name: paused
      datatype: boolean
      description: Create job in paused state
      mandatory: false
      default: false
    - name: wake_policy
      datatype: scheduler_types.wake_policy_t
      description: Whether to wake vehicle for this job
      mandatory: false
    - name: sleep_policy
      datatype: scheduler_types.sleep_policy_t
      description: Sleep behavior during execution
      mandatory: false
    - name: wake_lead_time_s
      datatype: uint32
      description: Seconds before scheduled_time to wake vehicle
      mandatory: false
    - name: authority
      datatype: scheduler_types.job_authority_t
      description: Job authority (CLOUD or VEHICLE) - used by sync bridge
      mandatory: false
    - name: cloud_seq
      datatype: uint64
      description: Cloud version sequence - used by sync bridge
      mandatory: false
    - name: vehicle_seq
      datatype: uint64
      description: Vehicle version sequence - used by sync bridge
      mandatory: false
    - name: deleted
      datatype: boolean
      description: Soft delete flag - used by sync bridge for tombstones
      mandatory: false
    - name: deleted_at_ms
      datatype: uint64
      description: Deletion timestamp - used by sync bridge
      mandatory: false
  - name: job_update_t
    description: Fields that can be updated in an existing job
    members:
    - name: title
      datatype: string
      description: New title
      mandatory: false
    - name: scheduled_time_ms
      datatype: uint64
      description: New scheduled time (epoch milliseconds UTC)
      mandatory: false
    - name: recurrence_rule
      datatype: string
      description: New recurrence rule
      mandatory: false
    - name: end_time_ms
      datatype: uint64
      description: New end time for recurring (epoch milliseconds UTC)
      mandatory: false
    - name: parameters_json
      datatype: string
      description: New JSON parameters
      mandatory: false
    - name: paused
      datatype: boolean
      description: New paused state
      mandatory: false
    - name: wake_policy
      datatype: scheduler_types.wake_policy_t
      description: New wake policy
      mandatory: false
    - name: sleep_policy
      datatype: scheduler_types.sleep_policy_t
      description: New sleep policy
      mandatory: false
    - name: wake_lead_time_s
      datatype: uint32
      description: New wake lead time in seconds
      mandatory: false
    - name: authority
      datatype: scheduler_types.job_authority_t
      description: Job authority - used by sync bridge
      mandatory: false
    - name: cloud_seq
      datatype: uint64
      description: Cloud version sequence - used by sync bridge
      mandatory: false
    - name: vehicle_seq
      datatype: uint64
      description: Vehicle version sequence - used by sync bridge
      mandatory: false
    - name: deleted
      datatype: boolean
      description: Soft delete flag - used by sync bridge for tombstones
      mandatory: false
    - name: deleted_at_ms
      datatype: uint64
      description: Deletion timestamp - used by sync bridge
      mandatory: false
  - name: execution_t
    description: 'Immutable record of a job execution. Executions are append-only
      facts - they never conflict.

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
      datatype: scheduler_types.job_status_t
      description: Result - JOB_STATUS_COMPLETED or JOB_STATUS_FAILED
    - name: result
      datatype: string
      description: JSON result (for completed jobs)
      mandatory: false
    - name: error_message
      datatype: string
      description: Error message (for failed jobs)
      mandatory: false
  - name: execution_filter_t
    description: 'Filter criteria for retrieving executions. All filters are optional
      - empty filter returns all executions.

      '
    members:
    - name: job_id
      datatype: string
      description: If set, only executions for this job
      mandatory: false
    - name: start_time_ms
      datatype: uint64
      description: If set, only executions with executed_at >= this value
      mandatory: false
    - name: end_time_ms
      datatype: uint64
      description: If set, only executions with executed_at <= this value
      mandatory: false
    - name: status
      datatype: scheduler_types.job_status_t
      description: If set (with has_status_filter=true), only executions with this
        status
      mandatory: false
    - name: has_status_filter
      datatype: boolean
      description: Whether to apply status filter
      mandatory: false
      default: false
    - name: limit
      datatype: int32
      description: Maximum number of executions to return (default 100)
      mandatory: false
    - name: offset
      datatype: int32
      description: Number of executions to skip (for pagination)
      mandatory: false
  methods:
  - name: create_job
    description: Create a new scheduled job
    input:
    - name: job
      datatype: job_create_t
      description: Job creation details
      mandatory: true
    output:
    - name: success
      datatype: boolean
      description: Whether creation succeeded
    - name: job_id
      datatype: string
      description: Unique identifier for the created job
    - name: message
      datatype: string
      description: Success or error message
  - name: list_jobs
    description: Retrieve jobs based on filter criteria (empty filter = all jobs)
    input:
    - name: filter
      datatype: job_filter_t
      description: Filter criteria (empty for all jobs including tombstones)
      mandatory: false
    output:
    - name: success
      datatype: boolean
      description: Whether request succeeded
    - name: jobs
      datatype: scheduler_types.job_t[]
      description: List of jobs matching criteria
  - name: list_jobs_hash
    description: 'Get a hash of the job state matching the filter criteria. Used for
      efficient change detection - poll this endpoint and only fetch full job list
      when hash changes. Uses same filter as list_jobs.

      '
    input:
    - name: filter
      datatype: job_filter_t
      description: Filter criteria (same as list_jobs)
      mandatory: false
    output:
    - name: state_hash
      datatype: uint64
      description: xxHash64 of the filtered job state (IDs, versions, statuses)
    - name: job_count
      datatype: int32
      description: Number of jobs matching the filter
  - name: get_job
    description: Retrieve a specific job by ID
    input:
    - name: job_id
      datatype: string
      description: Job identifier
      mandatory: true
    - name: include_deleted
      datatype: boolean
      description: If true, return job even if soft-deleted (tombstone). Default false.
      mandatory: false
      default: false
    output:
    - name: success
      datatype: boolean
      description: Whether job was found
    - name: job
      datatype: scheduler_types.job_t
      description: Job details
      mandatory: false
    - name: message
      datatype: string
      description: Error message if not found
      mandatory: false
  - name: update_job
    description: Update an existing job
    input:
    - name: job_id
      datatype: string
      description: Job identifier to update
      mandatory: true
    - name: updates
      datatype: job_update_t
      description: Fields to update
      mandatory: true
    output:
    - name: success
      datatype: boolean
      description: Whether update succeeded
    - name: message
      datatype: string
      description: Success or error message
  - name: delete_job
    description: Delete a scheduled job
    input:
    - name: job_id
      datatype: string
      description: Job identifier to delete
      mandatory: true
    output:
    - name: success
      datatype: boolean
      description: Whether deletion succeeded
    - name: message
      datatype: string
      description: Success or error message
  - name: pause_job
    description: Pause a scheduled job (sets paused=true)
    input:
    - name: job_id
      datatype: string
      description: Job identifier to pause
      mandatory: true
    output:
    - name: success
      datatype: boolean
      description: Whether pause succeeded
    - name: message
      datatype: string
      description: Success or error message
  - name: resume_job
    description: Resume a paused job (sets paused=false)
    input:
    - name: job_id
      datatype: string
      description: Job identifier to resume
      mandatory: true
    output:
    - name: success
      datatype: boolean
      description: Whether resume succeeded
    - name: message
      datatype: string
      description: Success or error message
  - name: trigger_job
    description: Trigger immediate execution of a job (ignores schedule)
    input:
    - name: job_id
      datatype: string
      description: Job identifier to trigger
      mandatory: true
    output:
    - name: success
      datatype: boolean
      description: Whether trigger succeeded
    - name: job
      datatype: scheduler_types.job_t
      description: Updated job with execution result
      mandatory: false
    - name: message
      datatype: string
      description: Success or error message
  - name: list_executions
    description: 'Retrieve execution history with optional filters. Returns immutable
      records of past job executions.

      '
    input:
    - name: filter
      datatype: execution_filter_t
      description: Filter criteria (empty for all executions)
      mandatory: false
    output:
    - name: success
      datatype: boolean
      description: Whether request succeeded
    - name: executions
      datatype: execution_t[]
      description: List of executions matching criteria
    - name: total_count
      datatype: int32
      description: Total matching executions (for pagination)
  - name: list_executions_hash
    description: 'Get a hash of the execution state matching the filter criteria.
      Used for efficient change detection - poll this endpoint and only fetch full
      execution list when hash changes.

      '
    input:
    - name: filter
      datatype: execution_filter_t
      description: Filter criteria (same as list_executions)
      mandatory: false
    output:
    - name: state_hash
      datatype: uint64
      description: xxHash64 of the filtered execution state
    - name: execution_count
      datatype: int32
      description: Number of executions matching the filter
  - name: set_job_remote_version
    description: 'Update the remote_version for a job. Called by sync bridge after
      receiving a job from cloud to record what version cloud has. This affects is_dirty()
      - job is dirty when local_version != remote_version.

      '
    input:
    - name: job_id
      datatype: string
      description: Job identifier
      mandatory: true
    - name: cloud_seq
      datatype: uint64
      description: Cloud sequence number from incoming job
      mandatory: true
    - name: vehicle_seq
      datatype: uint64
      description: Vehicle sequence number from incoming job
      mandatory: true
    output:
    - name: success
      datatype: boolean
      description: Whether update succeeded
    - name: message
      datatype: string
      description: Error message if failed
      mandatory: false
)IFEX";

}  // namespace ifex::schema
