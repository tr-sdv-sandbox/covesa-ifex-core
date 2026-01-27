// Canonical Job structure for Scheduler Sync Protocol v2
//
// This is the single source of truth for job state across:
// - Vehicle scheduler sync bridge
// - Cloud scheduler mirror
// - Wire format (via proto_adapter)
//
// All other representations should convert to/from this structure.

#pragma once

#include "version_vector.hpp"
#include <cstdint>
#include <string>

namespace ifex::scheduler {

// Job execution status
enum class JobStatus {
    PENDING = 0,
    RUNNING = 1,
    COMPLETED = 2,
    FAILED = 3,
    CANCELLED = 4
};

// Wake policy - whether job can wake vehicle from sleep
enum class WakePolicy {
    NO_WAKE = 0,      // Job skipped if vehicle is sleeping
    WAKE_REQUIRED = 1 // Job wakes vehicle if needed
};

// Sleep policy - whether job inhibits vehicle sleep
enum class SleepPolicy {
    NORMAL = 0,   // Normal sleep behavior
    INHIBIT = 1   // Inhibit sleep while job runs
};

// Source authority - determines conflict resolution winner
enum class JobAuthority {
    CLOUD = 0,    // Job created by cloud - cloud wins conflicts
    VEHICLE = 1   // Job created by vehicle - vehicle wins conflicts
};

// Sync state - derived by comparing local version with last confirmed remote version
enum class SyncState {
    PENDING = 0,  // My version differs from last confirmed remote version
    SYNCED = 1    // My version matches last confirmed remote version
};

// Canonical job structure - single source of truth
struct Job {
    // Identity
    std::string job_id;
    std::string vehicle_id;  // Owner vehicle

    // Content (included in hash)
    std::string title;
    std::string service;
    std::string method;
    std::string parameters_json;
    uint64_t scheduled_time_ms = 0;
    std::string recurrence_rule;
    uint64_t end_time_ms = 0;
    bool paused = false;
    WakePolicy wake_policy = WakePolicy::NO_WAKE;
    SleepPolicy sleep_policy = SleepPolicy::NORMAL;
    uint32_t wake_lead_time_s = 0;

    // Execution state (NOT included in content hash)
    JobStatus status = JobStatus::PENDING;
    uint64_t next_run_time_ms = 0;

    // Metadata (NOT included in content hash)
    uint64_t created_at_ms = 0;
    uint64_t updated_at_ms = 0;

    // Sync state
    VersionVector version;
    JobAuthority authority = JobAuthority::CLOUD;
    bool deleted = false;
    uint64_t deleted_at_ms = 0;
    SyncState sync_state = SyncState::PENDING;  // Derived: local vs last confirmed remote

    // Compute content hash (for change detection and quiescence)
    // Includes: job_id, title, service, method, parameters_json,
    //           scheduled_time_ms, recurrence_rule, end_time_ms,
    //           paused, wake_policy, sleep_policy, wake_lead_time_s
    // Excludes: status, next_run_time_ms, created_at_ms, updated_at_ms, version
    uint64_t content_hash() const;

    // Check if job is in terminal state
    bool is_terminal() const {
        return status == JobStatus::COMPLETED ||
               status == JobStatus::FAILED ||
               status == JobStatus::CANCELLED;
    }

    // Equality based on content (not metadata/sync state)
    bool content_equals(const Job& other) const {
        return content_hash() == other.content_hash();
    }
};

// String conversions for logging
const char* job_status_to_string(JobStatus status);
const char* wake_policy_to_string(WakePolicy policy);
const char* sleep_policy_to_string(SleepPolicy policy);
const char* job_authority_to_string(JobAuthority authority);

JobStatus job_status_from_string(const std::string& s);
WakePolicy wake_policy_from_string(const std::string& s);
SleepPolicy sleep_policy_from_string(const std::string& s);
JobAuthority job_authority_from_string(const std::string& s);

}  // namespace ifex::scheduler
