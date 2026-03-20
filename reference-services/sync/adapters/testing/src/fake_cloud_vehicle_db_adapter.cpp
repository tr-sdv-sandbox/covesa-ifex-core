#include "../include/fake_cloud_vehicle_db_adapter.hpp"

namespace ifex {
namespace sync {

std::vector<CanonicalRecord> InMemoryFakeAdapter::list_dirty_records(const DirtyRecordQuery& query) {
    std::vector<CanonicalRecord> out;
    for (const auto& kv : store_) {
        const CanonicalRecord& r = kv.second;
        if (r.locator.namespace_name != query.session.namespace_name) continue;
        out.push_back(r);
        if (out.size() >= query.limit) break;
    }
    return out;
}

ApplyResult InMemoryFakeAdapter::apply_record(const CanonicalRecord& record, const std::string& idempotency_key) {
    ApplyResult res;
    const std::string key = key_for(record.locator);
    auto it = store_.find(key);
    if (it != store_.end()) {
        auto ik = idempotency_.find(key);
        if (ik != idempotency_.end() && ik->second == idempotency_key) {
            res.disposition = ApplyDisposition::kDuplicate;
            res.durable_version = it->second.version_vector;
            return res;
        }
    }

    store_[key] = record;
    idempotency_[key] = idempotency_key;
    res.disposition = ApplyDisposition::kApplied;
    res.durable_version = record.version_vector;
    return res;
}

CheckpointReadResult InMemoryFakeAdapter::read_checkpoint(const SyncSessionKey& session) {
    CheckpointReadResult out;
    const std::string k = session.local_node_id + "|" + session.remote_node_id + "|" + session.namespace_name;
    auto it = checkpoints_.find(k);
    if (it != checkpoints_.end()) {
        out.found = true;
        out.checkpoint = it->second;
    }
    return out;
}

void InMemoryFakeAdapter::write_checkpoint(const SyncSessionKey& session, const CheckpointToken& checkpoint) {
    const std::string k = session.local_node_id + "|" + session.remote_node_id + "|" + session.namespace_name;
    checkpoints_[k] = checkpoint;
}

std::uint64_t InMemoryFakeAdapter::compute_state_checksum(const StateScope& scope) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const auto& kv : store_) {
        const CanonicalRecord& r = kv.second;
        if (r.locator.namespace_name != scope.namespace_name) continue;
        if (!scope.include_tombstones && r.operation == RecordOperation::kDelete) continue;
        for (auto b : r.locator.record_id) { h ^= static_cast<std::uint64_t>(b); h *= 1099511628211ULL; }
        h ^= r.version_vector.cloud_seq; h *= 1099511628211ULL;
        h ^= r.version_vector.truck_seq; h *= 1099511628211ULL;
        h ^= r.payload_checksum; h *= 1099511628211ULL;
        h ^= static_cast<std::uint64_t>(r.operation); h *= 1099511628211ULL;
    }
    return h;
}

std::vector<RecordLocator> InMemoryFakeAdapter::list_record_ids(const RecordIdQuery& query) {
    std::vector<RecordLocator> out;
    for (const auto& kv : store_) {
        const CanonicalRecord& r = kv.second;
        if (r.locator.namespace_name != query.namespace_name) continue;
        if (!query.include_tombstones && r.operation == RecordOperation::kDelete) continue;
        out.push_back(r.locator);
        if (query.limit != 0 && out.size() >= query.limit) break;
    }
    return out;
}

void InMemoryFakeAdapter::persist_conflict(const ConflictRecord& conflict) { conflicts_.push_back(conflict); }

std::vector<ConflictRecord> InMemoryFakeAdapter::query_conflicts(const ConflictQuery& query) {
    std::vector<ConflictRecord> out;
    for (const auto& c : conflicts_) {
        if (c.locator.namespace_name != query.namespace_name) continue;
        if (c.detected_at_ms < query.since_detected_at_ms) continue;
        if (!query.include_resolved && c.resolved) continue;
        out.push_back(c);
        if (out.size() >= query.limit) break;
    }
    return out;
}

std::vector<CanonicalRecord> InMemoryFakeAdapter::list_tombstones_for_gc(const TombstoneGcQuery& query) {
    std::vector<CanonicalRecord> out;
    for (const auto& kv : store_) {
        const CanonicalRecord& r = kv.second;
        if (r.locator.namespace_name != query.session.namespace_name) continue;
        if (r.operation != RecordOperation::kDelete) continue;
        if (r.tombstone_at_ms == 0) continue;
        if (r.tombstone_at_ms > query.retention_cutoff_ms) continue;
        out.push_back(r);
        if (out.size() >= query.limit) break;
    }
    return out;
}

std::string InMemoryFakeAdapter::key_for(const RecordLocator& loc) { return loc.namespace_name + "|" + loc.origin_node_id + "|" + std::string(loc.record_id.begin(), loc.record_id.end()); }

} // namespace sync
} // namespace ifex
