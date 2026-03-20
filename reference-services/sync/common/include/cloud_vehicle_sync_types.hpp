#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ifex {
namespace sync {

using ByteBuffer = std::vector<std::uint8_t>;

enum class RecordOperation {
    kCreate = 0,
    kUpdate = 1,
    kDelete = 2,
};

enum class ConflictClass {
    kConcurrentUpdate = 0,
    kNonOwnerMutation = 1,
    kStaleReplay = 2,
};

enum class ApplyDisposition {
    kApplied = 0,
    kDuplicate = 1,
    kStaleRejected = 2,
    kNonOwnerRejected = 3,
    kConflictPersisted = 4,
};

enum class CompareResult {
    kEqual = 0,
    kLocalDominates = 1,
    kRemoteDominates = 2,
    kConcurrent = 3,
};

struct VersionVector {
    std::uint64_t cloud_seq = 0;
    std::uint64_t truck_seq = 0;

    bool operator==(const VersionVector& other) const {
        return cloud_seq == other.cloud_seq && truck_seq == other.truck_seq;
    }

    bool operator!=(const VersionVector& other) const {
        return !(*this == other);
    }

    bool dominates(const VersionVector& other) const {
        return cloud_seq >= other.cloud_seq && truck_seq >= other.truck_seq &&
               (cloud_seq > other.cloud_seq || truck_seq > other.truck_seq);
    }

    static VersionVector merge(const VersionVector& lhs, const VersionVector& rhs) {
        VersionVector merged;
        merged.cloud_seq = std::max(lhs.cloud_seq, rhs.cloud_seq);
        merged.truck_seq = std::max(lhs.truck_seq, rhs.truck_seq);
        return merged;
    }
};

inline CompareResult compare_versions(const VersionVector& local,
                                      const VersionVector& remote) {
    if (local == remote) {
        return CompareResult::kEqual;
    }
    if (local.dominates(remote)) {
        return CompareResult::kLocalDominates;
    }
    if (remote.dominates(local)) {
        return CompareResult::kRemoteDominates;
    }
    return CompareResult::kConcurrent;
}

struct RecordLocator {
    ByteBuffer record_id;
    std::string namespace_name;
    std::string origin_node_id;
};

struct CheckpointToken {
    std::uint64_t sequence_number = 0;
    RecordLocator last_record;
    VersionVector last_version;
};

struct CheckpointReadResult {
    bool found = false;
    CheckpointToken checkpoint;
};

struct SyncSessionKey {
    std::string local_node_id;
    std::string remote_node_id;
    std::string namespace_name;
};

struct CanonicalRecord {
    RecordLocator locator;
    VersionVector version_vector;
    RecordOperation operation = RecordOperation::kCreate;
    ByteBuffer payload;
    std::uint32_t schema_version = 0;
    std::string idempotency_key;
    std::string correlation_id;
    std::uint64_t payload_checksum = 0;
    std::uint64_t wall_clock_ms = 0;
    std::uint64_t created_at_ms = 0;
    std::uint64_t updated_at_ms = 0;
    std::uint64_t tombstone_at_ms = 0;
    std::string tombstone_reason;
};

struct VersionAck {
    RecordLocator locator;
    VersionVector version_vector;
    std::string correlation_id;
    std::string idempotency_key;
};

struct ConflictRecord {
    RecordLocator locator;
    VersionVector local_version;
    VersionVector remote_version;
    ByteBuffer local_payload;
    ByteBuffer remote_payload;
    ConflictClass conflict_class = ConflictClass::kConcurrentUpdate;
    std::uint64_t detected_at_ms = 0;
    std::string correlation_id;
    bool resolved = false;
};

struct DirtyRecordQuery {
    SyncSessionKey session;
    std::size_t limit = 100;
    bool include_tombstones = true;
};

struct ConflictQuery {
    std::string namespace_name;
    std::uint64_t since_detected_at_ms = 0;
    bool include_resolved = false;
    std::size_t limit = 100;
};

struct StateScope {
    std::string namespace_name;
    bool include_tombstones = true;
};

struct RecordIdQuery {
    std::string namespace_name;
    bool include_tombstones = true;
    std::size_t limit = 0;
};

struct TombstoneGcQuery {
    SyncSessionKey session;
    std::uint64_t retention_cutoff_ms = 0;
    std::size_t limit = 100;
};

struct ApplyResult {
    ApplyDisposition disposition = ApplyDisposition::kApplied;
    VersionVector durable_version;
    bool has_persisted_conflict = false;
    ConflictRecord persisted_conflict;
};

}
}
