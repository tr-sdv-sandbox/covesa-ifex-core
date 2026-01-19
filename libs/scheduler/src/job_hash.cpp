// Job Hash implementation
//
// Uses the same algorithm as previously implemented in:
// - scheduler_sync_bridge.cpp (vehicle)
// - scheduler_store.cpp (cloud)
//
// Golden ratio constant 0x9e3779b9 for hash mixing (FNV-1a style)

#include "job_hash.hpp"
#include <functional>
#include <algorithm>
#include <stdexcept>

namespace ifex::scheduler {

// Hash mixing with golden ratio constant
uint64_t hash_mix(uint64_t h, uint64_t value) {
    h ^= value + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

uint64_t hash_mix_string(uint64_t h, const std::string& s) {
    std::hash<std::string> str_hash;
    return hash_mix(h, str_hash(s));
}

uint64_t compute_job_content_hash(const Job& job) {
    std::hash<std::string> str_hash;
    std::hash<uint64_t> uint64_hash;
    std::hash<bool> bool_hash;
    std::hash<int> int_hash;

    // Start with job_id
    uint64_t h = str_hash(job.job_id);

    // Mix in content fields (same order as original implementations)
    h ^= str_hash(job.title) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(job.service) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(job.method) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(job.parameters_json) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= uint64_hash(job.scheduled_time_ms) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= str_hash(job.recurrence_rule) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= uint64_hash(job.end_time_ms) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= bool_hash(job.paused) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= int_hash(static_cast<int>(job.wake_policy)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= int_hash(static_cast<int>(job.sleep_policy)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= uint64_hash(job.wake_lead_time_s) + 0x9e3779b9 + (h << 6) + (h >> 2);

    // NOTE: Excluded fields:
    // - status (execution state)
    // - next_run_time_ms (execution state)
    // - created_at_ms, updated_at_ms (metadata)
    // - version, authority (sync state)
    // - deleted, deleted_at_ms (tombstone state)

    return h;
}

uint64_t compute_state_checksum(const std::vector<Job>& jobs) {
    // xxHash64-style seed - return this for empty state to match original implementation
    constexpr uint64_t SEED = 0x9e3779b97f4a7c15ULL;

    if (jobs.empty()) {
        return SEED;
    }

    // Jobs must be sorted by job_id for deterministic results
    // Caller should ensure this, but we'll verify in debug
#ifndef NDEBUG
    for (size_t i = 1; i < jobs.size(); ++i) {
        if (jobs[i].job_id < jobs[i-1].job_id) {
            // Not sorted - this is a bug
            throw std::runtime_error("compute_state_checksum: jobs must be sorted by job_id");
        }
    }
#endif

    // xxHash64-style seed
    uint64_t hash = 0x9e3779b97f4a7c15ULL;

    for (const auto& job : jobs) {
        uint64_t job_hash = compute_job_content_hash(job);

        // Mix using FNV-1a style (same as original implementations)
        hash ^= job_hash;
        hash *= 0x100000001b3ULL;
    }

    return hash;
}

}  // namespace ifex::scheduler
