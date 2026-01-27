// Job Store - Abstract interface for job persistence
//
// Allows different backends:
// - MemoryStore: In-memory only (testing, vehicle cache)
// - PostgresAdapter: PostgreSQL (cloud persistence)
// - JsonAdapter: JSON file (vehicle persistence)
//
// All stores support the same operations, making it easy to:
// - Test with in-memory store
// - Cache in memory with write-through to persistence
// - Swap backends without changing business logic

#pragma once

#include "job.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <functional>

namespace ifex::scheduler {

// Query filter for listing jobs
struct JobFilter {
    std::string vehicle_id;       // Required for most queries
    std::optional<JobStatus> status;
    std::optional<std::string> service;
    bool include_deleted = false; // Include tombstones
    int limit = 100;
    int offset = 0;
};

// Result of a list operation
struct JobListResult {
    std::vector<Job> jobs;
    int total_count = 0;
    bool has_more = false;
};

// Abstract job store interface
class JobStore {
public:
    virtual ~JobStore() = default;

    // Get a single job by ID
    virtual std::optional<Job> get(const std::string& vehicle_id,
                                    const std::string& job_id) = 0;

    // Store or update a job
    virtual void put(const Job& job) = 0;

    // Delete a job (creates tombstone if soft_delete=true)
    virtual void remove(const std::string& vehicle_id,
                        const std::string& job_id,
                        bool soft_delete = true) = 0;

    // List jobs matching filter
    virtual JobListResult list(const JobFilter& filter) = 0;

    // Get all jobs for a vehicle (including tombstones if requested)
    virtual std::vector<Job> get_all(const std::string& vehicle_id,
                                      bool include_deleted = false) = 0;

    // Compute state checksum for a vehicle's jobs
    // Jobs are sorted by job_id for deterministic results
    virtual uint64_t state_checksum(const std::string& vehicle_id) = 0;

    // Check if any jobs need sync (have pending changes)
    virtual bool has_pending_sync(const std::string& vehicle_id) = 0;

    // Get jobs that need to be synced
    virtual std::vector<Job> get_pending_sync(const std::string& vehicle_id) = 0;

    // Mark jobs as synced
    virtual void mark_synced(const std::string& vehicle_id,
                              const std::vector<std::string>& job_ids) = 0;

    // Purge old tombstones (older than retention_days)
    virtual int purge_tombstones(int retention_days = 7) = 0;
};

// Observer interface for store changes
class JobStoreObserver {
public:
    virtual ~JobStoreObserver() = default;

    virtual void on_job_created(const Job& job) {}
    virtual void on_job_updated(const Job& old_job, const Job& new_job) {}
    virtual void on_job_deleted(const std::string& vehicle_id,
                                 const std::string& job_id) {}
};

// Factory function type for creating stores
using JobStoreFactory = std::function<std::unique_ptr<JobStore>()>;

}  // namespace ifex::scheduler
