// Memory Store implementation

#include "memory_store.hpp"
#include "job_hash.hpp"
#include <algorithm>

namespace ifex::scheduler {

MemoryStore::MemoryStore(const MemoryStoreConfig& config)
    : config_(config) {}

std::optional<Job> MemoryStore::get(const std::string& vehicle_id,
                                     const std::string& job_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto vit = vehicles_.find(vehicle_id);
    if (vit == vehicles_.end()) {
        return std::nullopt;
    }

    vit->second.last_access = std::chrono::steady_clock::now();

    auto jit = vit->second.jobs.find(job_id);
    if (jit == vit->second.jobs.end()) {
        return std::nullopt;
    }

    return jit->second;
}

void MemoryStore::put(const Job& job) {
    std::optional<Job> old_job;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& vdata = vehicles_[job.vehicle_id];
        vdata.last_access = std::chrono::steady_clock::now();

        auto it = vdata.jobs.find(job.job_id);
        if (it != vdata.jobs.end()) {
            old_job = it->second;
        }

        vdata.jobs[job.job_id] = job;

        if (config_.track_dirty) {
            vdata.dirty.insert(job.job_id);
        }

        maybe_evict_lru();
    }

    // Write-through to persistence
    if (config_.persistence) {
        config_.persistence->put(job);
    }

    // Notify observers
    if (old_job.has_value()) {
        notify_updated(old_job.value(), job);
    } else {
        notify_created(job);
    }
}

void MemoryStore::remove(const std::string& vehicle_id,
                          const std::string& job_id,
                          bool soft_delete) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto vit = vehicles_.find(vehicle_id);
        if (vit == vehicles_.end()) {
            return;
        }

        auto& vdata = vit->second;
        vdata.last_access = std::chrono::steady_clock::now();

        auto jit = vdata.jobs.find(job_id);
        if (jit == vdata.jobs.end()) {
            return;
        }

        if (soft_delete) {
            // Mark as tombstone
            jit->second.deleted = true;
            jit->second.deleted_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            if (config_.track_dirty) {
                vdata.dirty.insert(job_id);
            }
        } else {
            // Hard delete
            vdata.jobs.erase(jit);
            vdata.dirty.erase(job_id);
        }
    }

    // Write-through to persistence
    if (config_.persistence) {
        config_.persistence->remove(vehicle_id, job_id, soft_delete);
    }

    notify_deleted(vehicle_id, job_id);
}

JobListResult MemoryStore::list(const JobFilter& filter) {
    std::lock_guard<std::mutex> lock(mutex_);

    JobListResult result;

    auto vit = vehicles_.find(filter.vehicle_id);
    if (vit == vehicles_.end()) {
        return result;
    }

    vit->second.last_access = std::chrono::steady_clock::now();

    std::vector<Job> matched;
    for (const auto& [job_id, job] : vit->second.jobs) {
        // Filter by deleted
        if (job.deleted && !filter.include_deleted) {
            continue;
        }

        // Filter by status
        if (filter.status.has_value() && job.status != filter.status.value()) {
            continue;
        }

        // Filter by service
        if (filter.service.has_value() && job.service != filter.service.value()) {
            continue;
        }

        matched.push_back(job);
    }

    result.total_count = static_cast<int>(matched.size());

    // Sort by created_at_ms descending
    std::sort(matched.begin(), matched.end(),
              [](const Job& a, const Job& b) {
                  return a.created_at_ms > b.created_at_ms;
              });

    // Apply pagination
    int start = filter.offset;
    int end = std::min(start + filter.limit, static_cast<int>(matched.size()));

    if (start < static_cast<int>(matched.size())) {
        result.jobs.assign(matched.begin() + start, matched.begin() + end);
    }

    result.has_more = (end < static_cast<int>(matched.size()));

    return result;
}

std::vector<Job> MemoryStore::get_all(const std::string& vehicle_id,
                                       bool include_deleted) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Job> result;

    auto vit = vehicles_.find(vehicle_id);
    if (vit == vehicles_.end()) {
        return result;
    }

    vit->second.last_access = std::chrono::steady_clock::now();

    for (const auto& [job_id, job] : vit->second.jobs) {
        if (!include_deleted && job.deleted) {
            continue;
        }
        result.push_back(job);
    }

    // Sort by job_id for deterministic order
    std::sort(result.begin(), result.end(),
              [](const Job& a, const Job& b) {
                  return a.job_id < b.job_id;
              });

    return result;
}

uint64_t MemoryStore::state_checksum(const std::string& vehicle_id) {
    // Get all jobs sorted by job_id
    auto jobs = get_all(vehicle_id, true);  // Include deleted for checksum

    return compute_state_checksum(jobs);
}

bool MemoryStore::has_pending_sync(const std::string& vehicle_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto vit = vehicles_.find(vehicle_id);
    if (vit == vehicles_.end()) {
        return false;
    }

    return !vit->second.dirty.empty();
}

std::vector<Job> MemoryStore::get_pending_sync(const std::string& vehicle_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Job> result;

    auto vit = vehicles_.find(vehicle_id);
    if (vit == vehicles_.end()) {
        return result;
    }

    for (const auto& job_id : vit->second.dirty) {
        auto jit = vit->second.jobs.find(job_id);
        if (jit != vit->second.jobs.end()) {
            result.push_back(jit->second);
        }
    }

    return result;
}

void MemoryStore::mark_synced(const std::string& vehicle_id,
                               const std::vector<std::string>& job_ids) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto vit = vehicles_.find(vehicle_id);
    if (vit == vehicles_.end()) {
        return;
    }

    for (const auto& job_id : job_ids) {
        vit->second.dirty.erase(job_id);
    }
}

int MemoryStore::purge_tombstones(int retention_days) {
    std::lock_guard<std::mutex> lock(mutex_);

    // If retention is 0, purge all tombstones regardless of age
    bool purge_all = (retention_days <= 0);

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    uint64_t retention_ms = static_cast<uint64_t>(retention_days) * 24 * 60 * 60 * 1000;
    uint64_t cutoff_ms = now_ms - retention_ms;

    int purged = 0;

    for (auto& [vehicle_id, vdata] : vehicles_) {
        std::vector<std::string> to_remove;

        for (const auto& [job_id, job] : vdata.jobs) {
            if (job.deleted && (purge_all || job.deleted_at_ms < cutoff_ms)) {
                to_remove.push_back(job_id);
            }
        }

        for (const auto& job_id : to_remove) {
            vdata.jobs.erase(job_id);
            vdata.dirty.erase(job_id);
            purged++;
        }
    }

    return purged;
}

void MemoryStore::add_observer(std::shared_ptr<JobStoreObserver> observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    observers_.push_back(observer);
}

void MemoryStore::remove_observer(std::shared_ptr<JobStoreObserver> observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    observers_.erase(
        std::remove(observers_.begin(), observers_.end(), observer),
        observers_.end());
}

void MemoryStore::load_vehicle(const std::string& vehicle_id) {
    if (!config_.persistence) {
        return;
    }

    auto jobs = config_.persistence->get_all(vehicle_id, true);

    std::lock_guard<std::mutex> lock(mutex_);

    auto& vdata = vehicles_[vehicle_id];
    vdata.last_access = std::chrono::steady_clock::now();

    for (auto& job : jobs) {
        vdata.jobs[job.job_id] = std::move(job);
    }
    // Loaded jobs are not dirty (they came from persistence)
    vdata.dirty.clear();
}

void MemoryStore::flush(const std::string& vehicle_id) {
    if (!config_.persistence) {
        return;
    }

    std::vector<Job> to_flush;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto vit = vehicles_.find(vehicle_id);
        if (vit == vehicles_.end()) {
            return;
        }

        for (const auto& job_id : vit->second.dirty) {
            auto jit = vit->second.jobs.find(job_id);
            if (jit != vit->second.jobs.end()) {
                to_flush.push_back(jit->second);
            }
        }

        vit->second.dirty.clear();
    }

    for (const auto& job : to_flush) {
        config_.persistence->put(job);
    }
}

void MemoryStore::flush_all() {
    if (!config_.persistence) {
        return;
    }

    std::vector<std::string> vehicle_ids;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [vid, _] : vehicles_) {
            vehicle_ids.push_back(vid);
        }
    }

    for (const auto& vid : vehicle_ids) {
        flush(vid);
    }
}

MemoryStore::Stats MemoryStore::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    stats.total_vehicles = vehicles_.size();

    for (const auto& [vid, vdata] : vehicles_) {
        stats.total_jobs += vdata.jobs.size();
        stats.dirty_jobs += vdata.dirty.size();

        for (const auto& [jid, job] : vdata.jobs) {
            if (job.deleted) {
                stats.tombstones++;
            }
        }
    }

    return stats;
}

void MemoryStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    vehicles_.clear();
}

void MemoryStore::maybe_evict_lru() {
    if (config_.max_vehicles == 0 || vehicles_.size() <= config_.max_vehicles) {
        return;
    }

    // Find LRU vehicle
    auto oldest = vehicles_.end();
    auto oldest_time = std::chrono::steady_clock::time_point::max();

    for (auto it = vehicles_.begin(); it != vehicles_.end(); ++it) {
        if (it->second.last_access < oldest_time) {
            oldest_time = it->second.last_access;
            oldest = it;
        }
    }

    if (oldest != vehicles_.end()) {
        // Flush before eviction if persistence configured
        if (config_.persistence && !oldest->second.dirty.empty()) {
            for (const auto& job_id : oldest->second.dirty) {
                auto jit = oldest->second.jobs.find(job_id);
                if (jit != oldest->second.jobs.end()) {
                    config_.persistence->put(jit->second);
                }
            }
        }
        vehicles_.erase(oldest);
    }
}

void MemoryStore::notify_created(const Job& job) {
    // Copy observers to avoid holding lock during callbacks
    std::vector<std::shared_ptr<JobStoreObserver>> obs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        obs = observers_;
    }

    for (auto& o : obs) {
        o->on_job_created(job);
    }
}

void MemoryStore::notify_updated(const Job& old_job, const Job& new_job) {
    std::vector<std::shared_ptr<JobStoreObserver>> obs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        obs = observers_;
    }

    for (auto& o : obs) {
        o->on_job_updated(old_job, new_job);
    }
}

void MemoryStore::notify_deleted(const std::string& vehicle_id,
                                  const std::string& job_id) {
    std::vector<std::shared_ptr<JobStoreObserver>> obs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        obs = observers_;
    }

    for (auto& o : obs) {
        o->on_job_deleted(vehicle_id, job_id);
    }
}

}  // namespace ifex::scheduler
