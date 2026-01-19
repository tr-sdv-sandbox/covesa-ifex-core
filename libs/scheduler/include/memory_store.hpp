// Memory Store - In-memory implementation of JobStore
//
// Features:
// - Fast in-memory operations
// - Thread-safe with mutex
// - Tracks dirty state for sync
// - Optional write-through to persistence backend
// - Observer support for change notifications
//
// Use cases:
// - Testing (no persistence needed)
// - Vehicle side (with JSON file write-through)
// - Cloud side (with PostgreSQL write-through + LRU eviction)

#pragma once

#include "job_store.hpp"
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>

namespace ifex::scheduler {

// Configuration for MemoryStore
struct MemoryStoreConfig {
    // Maximum number of vehicles to cache (0 = unlimited)
    size_t max_vehicles = 0;

    // Whether to track dirty state for sync
    bool track_dirty = true;

    // Optional persistence backend for write-through
    std::shared_ptr<JobStore> persistence;
};

// In-memory job store implementation
class MemoryStore : public JobStore {
public:
    explicit MemoryStore(const MemoryStoreConfig& config = {});
    ~MemoryStore() override = default;

    // JobStore interface
    std::optional<Job> get(const std::string& vehicle_id,
                            const std::string& job_id) override;

    void put(const Job& job) override;

    void remove(const std::string& vehicle_id,
                const std::string& job_id,
                bool soft_delete = true) override;

    JobListResult list(const JobFilter& filter) override;

    std::vector<Job> get_all(const std::string& vehicle_id,
                              bool include_deleted = false) override;

    uint64_t state_checksum(const std::string& vehicle_id) override;

    bool has_pending_sync(const std::string& vehicle_id) override;

    std::vector<Job> get_pending_sync(const std::string& vehicle_id) override;

    void mark_synced(const std::string& vehicle_id,
                      const std::vector<std::string>& job_ids) override;

    int purge_tombstones(int retention_days = 7) override;

    // Additional methods for MemoryStore

    // Add observer for change notifications
    void add_observer(std::shared_ptr<JobStoreObserver> observer);

    // Remove observer
    void remove_observer(std::shared_ptr<JobStoreObserver> observer);

    // Load all jobs for a vehicle from persistence (if configured)
    void load_vehicle(const std::string& vehicle_id);

    // Flush dirty jobs to persistence (if configured)
    void flush(const std::string& vehicle_id);
    void flush_all();

    // Get statistics
    struct Stats {
        size_t total_jobs = 0;
        size_t total_vehicles = 0;
        size_t dirty_jobs = 0;
        size_t tombstones = 0;
    };
    Stats get_stats() const;

    // Clear all data (for testing)
    void clear();

private:
    // Per-vehicle job storage
    struct VehicleData {
        std::unordered_map<std::string, Job> jobs;  // job_id -> Job
        std::unordered_set<std::string> dirty;      // job_ids that need sync
        std::chrono::steady_clock::time_point last_access;
    };

    // Evict least recently used vehicle if over capacity
    void maybe_evict_lru();

    // Notify observers
    void notify_created(const Job& job);
    void notify_updated(const Job& old_job, const Job& new_job);
    void notify_deleted(const std::string& vehicle_id, const std::string& job_id);

    MemoryStoreConfig config_;
    mutable std::mutex mutex_;

    // vehicle_id -> VehicleData
    std::unordered_map<std::string, VehicleData> vehicles_;

    // Observers
    std::vector<std::shared_ptr<JobStoreObserver>> observers_;
};

}  // namespace ifex::scheduler
