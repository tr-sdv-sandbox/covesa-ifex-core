#pragma once

#include "../../../common/include/cloud_vehicle_db_adapter.hpp"
#include "../../../common/include/cloud_vehicle_sync_core.hpp"

#include <map>
#include <mutex>
#include <string>

struct sqlite3;

namespace ifex {
namespace sync {

struct DatabaseAdapterConfig {
    std::string database_path;
    std::map<std::string, RecordOwner> namespace_owners;
    RecordOwner default_owner = RecordOwner::kShared;
};

class SqliteCloudVehicleDbAdapter : public CloudVehicleDbAdapter {
public:
    explicit SqliteCloudVehicleDbAdapter(DatabaseAdapterConfig config);
    ~SqliteCloudVehicleDbAdapter() noexcept override;

    SqliteCloudVehicleDbAdapter(const SqliteCloudVehicleDbAdapter&) = delete;
    SqliteCloudVehicleDbAdapter& operator=(const SqliteCloudVehicleDbAdapter&) = delete;
    SqliteCloudVehicleDbAdapter(SqliteCloudVehicleDbAdapter&&) = delete;
    SqliteCloudVehicleDbAdapter& operator=(SqliteCloudVehicleDbAdapter&&) = delete;

    std::vector<CanonicalRecord> list_dirty_records(const DirtyRecordQuery& query) override;
    ApplyResult apply_record(const CanonicalRecord& record,
                             const std::string& idempotency_key) override;
    CheckpointReadResult read_checkpoint(const SyncSessionKey& session) override;
    void write_checkpoint(const SyncSessionKey& session,
                          const CheckpointToken& checkpoint) override;
    std::uint64_t compute_state_checksum(const StateScope& scope) override;
    std::vector<RecordLocator> list_record_ids(const RecordIdQuery& query) override;
    void persist_conflict(const ConflictRecord& conflict) override;
    std::vector<ConflictRecord> query_conflicts(const ConflictQuery& query) override;
    std::vector<CanonicalRecord> list_tombstones_for_gc(const TombstoneGcQuery& query) override;

private:
    RecordOwner owner_for(const std::string& namespace_name) const;
    std::uint64_t conflict_detected_at_ms(const CanonicalRecord& record) const;
    void initialize_schema();

    DatabaseAdapterConfig config_;
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
};

}
}
