#pragma once

#include "cloud_vehicle_sync_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ifex {
namespace sync {

enum class RecordOwner {
    kCloud = 0,
    kTruck = 1,
    kShared = 2,
};

struct ResolveOutcome {
    ApplyDisposition disposition = ApplyDisposition::kApplied;
    bool should_apply = false;
    bool should_persist_conflict = false;
    bool is_replay = false;
    bool is_tombstone_replay = false;
    bool checkpoint_safe = false;
    ConflictRecord conflict_record;
};

struct AckProcessingResult {
    std::vector<VersionAck> accepted_acks;
    std::vector<VersionAck> replayed_acks;
    CheckpointToken next_checkpoint;
    bool checkpoint_advanced = false;
};

struct GapRecoveryDecision {
    bool trigger_gap_recovery = false;
    std::string reason;
};

class CloudVehicleSyncCore {
public:
    static ResolveOutcome resolve_remote_record(const CanonicalRecord& remote_record,
                                                const CanonicalRecord* local_record,
                                                RecordOwner owner,
                                                const std::string& remote_sender_node_id,
                                                std::uint64_t detected_at_ms);

    static ConflictRecord make_conflict_record(const RecordLocator& locator,
                                               const VersionVector& local_version,
                                               const VersionVector& remote_version,
                                               const ByteBuffer& local_payload,
                                               const ByteBuffer& remote_payload,
                                               ConflictClass conflict_class,
                                               std::uint64_t detected_at_ms,
                                               const std::string& correlation_id);

    static bool is_tombstone(const CanonicalRecord& record);

    static CheckpointToken advance_checkpoint_monotonic(const CheckpointToken& current,
                                                        const RecordLocator& last_record,
                                                        const VersionVector& last_version,
                                                        bool should_advance);

    static AckProcessingResult process_acks(const std::vector<VersionAck>& incoming_acks,
                                            const std::vector<VersionAck>& known_acks,
                                            const CheckpointToken& current_checkpoint);

    static GapRecoveryDecision decide_gap_recovery(std::uint64_t local_checksum,
                                                   std::uint64_t remote_checksum,
                                                   bool has_dirty_records);
};

}
}
