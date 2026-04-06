#pragma once

#include "cloud_vehicle_sync_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ifex {
namespace sync {

class CloudVehicleDbAdapter {
public:
    virtual ~CloudVehicleDbAdapter() = default;

    virtual std::vector<CanonicalRecord> list_dirty_records(
        const DirtyRecordQuery& query) = 0;

    virtual ApplyResult apply_record(
        const CanonicalRecord& record,
        const std::string& idempotency_key,
        const std::string& sender_node_id = "") = 0;

    virtual CheckpointReadResult read_checkpoint(
        const SyncSessionKey& session) = 0;
    virtual void write_checkpoint(
        const SyncSessionKey& session,
        const CheckpointToken& checkpoint) = 0;
    virtual void persist_remote_acks(
        const SyncSessionKey& session,
        const std::vector<VersionAck>& durable_acks) = 0;
    virtual std::vector<VersionAck> list_remote_acks(
        const SyncSessionKey& session) = 0;

    virtual std::uint64_t compute_state_checksum(
        const StateScope& scope) = 0;

    virtual std::vector<RecordLocator> list_record_ids(
        const RecordIdQuery& query) = 0;

    virtual void persist_conflict(const ConflictRecord& conflict) = 0;
    virtual std::vector<ConflictRecord> query_conflicts(
        const ConflictQuery& query) = 0;

    virtual std::vector<CanonicalRecord> list_tombstones_for_gc(
        const TombstoneGcQuery& query) = 0;
};

}
}
