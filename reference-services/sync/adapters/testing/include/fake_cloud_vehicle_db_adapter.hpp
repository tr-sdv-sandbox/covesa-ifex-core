// In-memory fake CloudVehicleDbAdapter for tests (header)
#pragma once

#include "../../../common/include/cloud_vehicle_db_adapter.hpp"
#include <map>
#include <string>
#include <vector>

namespace ifex {
namespace sync {

class InMemoryFakeAdapter : public CloudVehicleDbAdapter {
public:
    InMemoryFakeAdapter() = default;
    ~InMemoryFakeAdapter() override = default;

    std::vector<CanonicalRecord> list_dirty_records(const DirtyRecordQuery& query) override;
    ApplyResult apply_record(const CanonicalRecord& record,
                             const std::string& idempotency_key,
                             const std::string& sender_node_id = "") override;
    CheckpointReadResult read_checkpoint(const SyncSessionKey& session) override;
    void write_checkpoint(const SyncSessionKey& session, const CheckpointToken& checkpoint) override;
    void persist_remote_acks(const SyncSessionKey& session,
                             const std::vector<VersionAck>& durable_acks) override;
    std::vector<VersionAck> list_remote_acks(const SyncSessionKey& session) override;
    std::uint64_t compute_state_checksum(const StateScope& scope) override;
    std::vector<RecordLocator> list_record_ids(const RecordIdQuery& query) override;
    void persist_conflict(const ConflictRecord& conflict) override;
    std::vector<ConflictRecord> query_conflicts(const ConflictQuery& query) override;
    std::vector<CanonicalRecord> list_tombstones_for_gc(const TombstoneGcQuery& query) override;

private:
    static std::string key_for(const RecordLocator& loc);
    std::map<std::string, CanonicalRecord> store_;
    std::map<std::string, std::string> idempotency_;
    std::map<std::string, CheckpointToken> checkpoints_;
    std::map<std::string, VersionVector> remote_acks_;
    std::vector<ConflictRecord> conflicts_;
};

} // namespace sync
} // namespace ifex
