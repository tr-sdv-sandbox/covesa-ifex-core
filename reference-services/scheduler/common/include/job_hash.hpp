// Job Hash computation for Scheduler Sync Protocol v2
//
// Provides consistent hash computation for:
// - Content change detection (single job)
// - State checksum for quiescence detection (all jobs)
//
// IMPORTANT: This implementation must be identical on vehicle and cloud
// for quiescence detection to work correctly.

#pragma once

#include "job.hpp"
#include <vector>
#include <cstdint>

namespace ifex::scheduler {

// Compute content hash for a single job.
// Used to detect if job content has changed.
//
// Included fields: job_id, title, service, method, parameters_json,
//                  scheduled_time_ms, recurrence_rule, end_time_ms,
//                  paused, wake_policy, sleep_policy, wake_lead_time_s
//
// Excluded fields: status, next_run_time_ms, created_at_ms, updated_at_ms,
//                  version, authority, deleted, deleted_at_ms
uint64_t compute_job_content_hash(const Job& job);

// Compute state checksum for a collection of jobs.
// Used for quiescence detection - when cloud and vehicle checksums match,
// no sync traffic is needed.
//
// Jobs MUST be sorted by job_id for deterministic results.
// Deleted jobs (tombstones) are included in checksum.
uint64_t compute_state_checksum(const std::vector<Job>& jobs);

// Hash mixing function (FNV-1a style with golden ratio)
// Used internally but exposed for testing
uint64_t hash_mix(uint64_t h, uint64_t value);
uint64_t hash_mix_string(uint64_t h, const std::string& s);

}  // namespace ifex::scheduler
