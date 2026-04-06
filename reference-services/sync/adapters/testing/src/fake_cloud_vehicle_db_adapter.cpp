#include "../include/fake_cloud_vehicle_db_adapter.hpp"

#include "../../../common/include/cloud_vehicle_sync_core.hpp"

#include <cstdint>

namespace ifex {
namespace sync {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix_bytes(std::uint64_t& hash, const ByteBuffer& value) {
    for (const std::uint8_t byte : value) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kFnvPrime;
    }
}

void mix_string(std::uint64_t& hash, const std::string& value) {
    mix_bytes(hash, ByteBuffer(value.begin(), value.end()));
}

void mix_u64(std::uint64_t& hash, std::uint64_t value) {
    for (int shift = 0; shift < 8; ++shift) {
        hash ^= static_cast<std::uint64_t>((value >> (shift * 8)) & 0xFFU);
        hash *= kFnvPrime;
    }
}

void mix_u32(std::uint64_t& hash, std::uint32_t value) {
    for (int shift = 0; shift < 4; ++shift) {
        hash ^= static_cast<std::uint64_t>((value >> (shift * 8)) & 0xFFU);
        hash *= kFnvPrime;
    }
}

std::string session_prefix(const SyncSessionKey& session) {
    return session.local_node_id + "|" + session.remote_node_id + "|" + session.namespace_name;
}

RecordOwner owner_for_namespace(const std::string& namespace_name) {
    if (namespace_name == "cloud-owned") {
        return RecordOwner::kCloud;
    }
    if (namespace_name == "truck-owned") {
        return RecordOwner::kTruck;
    }
    return RecordOwner::kShared;
}

std::uint64_t conflict_detected_at_ms(const CanonicalRecord& record) {
    if (record.updated_at_ms != 0) {
        return record.updated_at_ms;
    }
    if (record.tombstone_at_ms != 0) {
        return record.tombstone_at_ms;
    }
    if (record.wall_clock_ms != 0) {
        return record.wall_clock_ms;
    }
    return record.created_at_ms;
}

}

std::vector<CanonicalRecord> InMemoryFakeAdapter::list_dirty_records(const DirtyRecordQuery& query) {
    std::vector<CanonicalRecord> out;
    for (const auto& kv : store_) {
        const CanonicalRecord& r = kv.second;
        if (r.locator.namespace_name != query.session.namespace_name) continue;
        const std::string ack_key = session_prefix(query.session) + "|" + key_for(r.locator);
        const auto ack_it = remote_acks_.find(ack_key);
        if (ack_it != remote_acks_.end() && ack_it->second == r.version_vector) continue;
        out.push_back(r);
        if (query.limit != 0 && out.size() >= query.limit) break;
    }
    return out;
}

ApplyResult InMemoryFakeAdapter::apply_record(const CanonicalRecord& record,
                                              const std::string& idempotency_key,
                                              const std::string& sender_node_id) {
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

    const CanonicalRecord* local_record = it != store_.end() ? &it->second : nullptr;
    const ResolveOutcome outcome = CloudVehicleSyncCore::resolve_remote_record(
        record,
        local_record,
        owner_for_namespace(record.locator.namespace_name),
        sender_node_id.empty() ? record.locator.origin_node_id : sender_node_id,
        conflict_detected_at_ms(record));

    res.disposition = outcome.disposition;
    res.durable_version = local_record != nullptr ? local_record->version_vector : VersionVector();

    if (outcome.should_apply) {
        store_[key] = record;
        idempotency_[key] = idempotency_key;
        res.durable_version = record.version_vector;
    }

    if (outcome.should_persist_conflict) {
        conflicts_.push_back(outcome.conflict_record);
        res.has_persisted_conflict = true;
        res.persisted_conflict = outcome.conflict_record;
    }

    if (!outcome.should_apply && local_record == nullptr) {
        res.durable_version = record.version_vector;
    }

    return res;
}

CheckpointReadResult InMemoryFakeAdapter::read_checkpoint(const SyncSessionKey& session) {
    CheckpointReadResult out;
    const std::string k = session_prefix(session);
    auto it = checkpoints_.find(k);
    if (it != checkpoints_.end()) {
        out.found = true;
        out.checkpoint = it->second;
    }
    return out;
}

void InMemoryFakeAdapter::write_checkpoint(const SyncSessionKey& session, const CheckpointToken& checkpoint) {
    const std::string k = session_prefix(session);
    const auto it = checkpoints_.find(k);
    if (it != checkpoints_.end() && checkpoint.sequence_number < it->second.sequence_number) {
        return;
    }
    checkpoints_[k] = checkpoint;
}

void InMemoryFakeAdapter::persist_remote_acks(const SyncSessionKey& session,
                                              const std::vector<VersionAck>& durable_acks) {
    const std::string prefix = session_prefix(session);
    for (const VersionAck& ack : durable_acks) {
        const std::string ack_key = prefix + "|" + key_for(ack.locator);
        const auto existing = remote_acks_.find(ack_key);
        if (existing != remote_acks_.end()) {
            const CompareResult cmp = compare_versions(existing->second, ack.version_vector);
            if (cmp == CompareResult::kEqual || cmp == CompareResult::kLocalDominates) {
                continue;
            }
        }
        remote_acks_[ack_key] = ack.version_vector;
    }
}

std::vector<VersionAck> InMemoryFakeAdapter::list_remote_acks(const SyncSessionKey& session) {
    std::vector<VersionAck> durable_acks;
    const std::string prefix = session_prefix(session) + "|";
    for (const auto& kv : remote_acks_) {
        if (kv.first.rfind(prefix, 0) != 0) {
            continue;
        }

        const std::string locator = kv.first.substr(prefix.size());
        const std::size_t first_sep = locator.find('|');
        if (first_sep == std::string::npos) {
            continue;
        }
        const std::size_t second_sep = locator.find('|', first_sep + 1);
        if (second_sep == std::string::npos) {
            continue;
        }

        VersionAck ack;
        ack.locator.namespace_name = locator.substr(0, first_sep);
        ack.locator.origin_node_id = locator.substr(first_sep + 1, second_sep - first_sep - 1);
        const std::string record_id = locator.substr(second_sep + 1);
        ack.locator.record_id.assign(record_id.begin(), record_id.end());
        ack.version_vector = kv.second;
        durable_acks.push_back(std::move(ack));
    }
    return durable_acks;
}

std::uint64_t InMemoryFakeAdapter::compute_state_checksum(const StateScope& scope) {
    std::uint64_t h = kFnvOffsetBasis;
    for (const auto& kv : store_) {
        const CanonicalRecord& r = kv.second;
        if (r.locator.namespace_name != scope.namespace_name) continue;
        if (!scope.include_tombstones && r.operation == RecordOperation::kDelete) continue;
        mix_string(h, r.locator.namespace_name);
        mix_string(h, r.locator.origin_node_id);
        mix_bytes(h, r.locator.record_id);
        mix_u64(h, r.version_vector.cloud_seq);
        mix_u64(h, r.version_vector.truck_seq);
        mix_u32(h, static_cast<std::uint32_t>(r.operation));
        mix_bytes(h, r.payload);
        mix_u32(h, r.schema_version);
        mix_u32(h, r.operation == RecordOperation::kDelete ? static_cast<std::uint32_t>(1)
                                                            : static_cast<std::uint32_t>(0));
        mix_string(h, r.tombstone_reason);
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
    const std::string prefix = session_prefix(query.session);
    for (const auto& kv : store_) {
        const CanonicalRecord& r = kv.second;
        if (r.locator.namespace_name != query.session.namespace_name) continue;
        if (r.operation != RecordOperation::kDelete) continue;
        if (r.tombstone_at_ms == 0) continue;
        if (r.tombstone_at_ms > query.retention_cutoff_ms) continue;
        const auto ack_it = remote_acks_.find(prefix + "|" + key_for(r.locator));
        if (ack_it == remote_acks_.end() || ack_it->second != r.version_vector) continue;
        out.push_back(r);
        if (query.limit != 0 && out.size() >= query.limit) break;
    }
    return out;
}

std::string InMemoryFakeAdapter::key_for(const RecordLocator& loc) { return loc.namespace_name + "|" + loc.origin_node_id + "|" + std::string(loc.record_id.begin(), loc.record_id.end()); }

} // namespace sync
} // namespace ifex
