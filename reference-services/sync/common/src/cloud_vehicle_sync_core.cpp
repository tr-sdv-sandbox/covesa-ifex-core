#include "../include/cloud_vehicle_sync_core.hpp"

namespace ifex {
namespace sync {
namespace {

bool locator_equals(const RecordLocator& lhs, const RecordLocator& rhs) {
    return lhs.record_id == rhs.record_id &&
           lhs.namespace_name == rhs.namespace_name &&
           lhs.origin_node_id == rhs.origin_node_id;
}

bool version_ack_equals(const VersionAck& lhs, const VersionAck& rhs) {
    return locator_equals(lhs.locator, rhs.locator) &&
           lhs.version_vector == rhs.version_vector;
}

bool has_ack(const std::vector<VersionAck>& acks, const VersionAck& candidate) {
    for (const VersionAck& ack : acks) {
        if (version_ack_equals(ack, candidate)) {
            return true;
        }
    }
    return false;
}

bool is_remote_sender_authorized(RecordOwner owner, const std::string& remote_sender_node_id) {
    if (owner == RecordOwner::kCloud) {
        return remote_sender_node_id == "cloud";
    }
    if (owner == RecordOwner::kTruck) {
        return remote_sender_node_id != "cloud";
    }
    return true;
}

bool same_logical_content(const CanonicalRecord& lhs, const CanonicalRecord& rhs) {
    return lhs.operation == rhs.operation &&
           lhs.schema_version == rhs.schema_version &&
           lhs.payload == rhs.payload &&
           lhs.locator.record_id == rhs.locator.record_id &&
           lhs.locator.namespace_name == rhs.locator.namespace_name &&
           lhs.locator.origin_node_id == rhs.locator.origin_node_id;
}

}

ResolveOutcome CloudVehicleSyncCore::resolve_remote_record(const CanonicalRecord& remote_record,
                                                         const CanonicalRecord* local_record,
                                                         RecordOwner owner,
                                                         const std::string& remote_sender_node_id,
                                                         std::uint64_t detected_at_ms) {
    ResolveOutcome outcome;

    if (!is_remote_sender_authorized(owner, remote_sender_node_id)) {
        outcome.disposition = ApplyDisposition::kNonOwnerRejected;
        outcome.should_apply = false;
        outcome.should_persist_conflict = true;
        outcome.checkpoint_safe = true;
        outcome.conflict_record = make_conflict_record(
            remote_record.locator,
            local_record != nullptr ? local_record->version_vector : VersionVector(),
            remote_record.version_vector,
            local_record != nullptr ? local_record->payload : ByteBuffer(),
            remote_record.payload,
            ConflictClass::kNonOwnerMutation,
            detected_at_ms,
            remote_record.correlation_id);
        return outcome;
    }

    if (local_record == nullptr) {
        outcome.disposition = ApplyDisposition::kApplied;
        outcome.should_apply = true;
        outcome.checkpoint_safe = true;
        return outcome;
    }

    const CompareResult cmp = compare_versions(local_record->version_vector,
                                               remote_record.version_vector);
    if (cmp == CompareResult::kEqual) {
        if (same_logical_content(*local_record, remote_record)) {
            outcome.disposition = ApplyDisposition::kDuplicate;
            outcome.should_apply = false;
            outcome.is_replay = true;
            outcome.is_tombstone_replay = is_tombstone(*local_record) && is_tombstone(remote_record);
            outcome.checkpoint_safe = true;
            return outcome;
        }

        outcome.disposition = ApplyDisposition::kConflictPersisted;
        outcome.should_apply = false;
        outcome.should_persist_conflict = true;
        outcome.checkpoint_safe = true;
        outcome.conflict_record = make_conflict_record(
            remote_record.locator,
            local_record->version_vector,
            remote_record.version_vector,
            local_record->payload,
            remote_record.payload,
            ConflictClass::kConcurrentUpdate,
            detected_at_ms,
            remote_record.correlation_id);
        return outcome;
    }

    if (cmp == CompareResult::kRemoteDominates) {
        outcome.disposition = ApplyDisposition::kApplied;
        outcome.should_apply = true;
        outcome.checkpoint_safe = true;
        return outcome;
    }

    if (cmp == CompareResult::kLocalDominates) {
        outcome.disposition = ApplyDisposition::kStaleRejected;
        outcome.should_apply = false;
        outcome.is_replay = true;
        outcome.should_persist_conflict = true;
        outcome.checkpoint_safe = true;
        outcome.conflict_record = make_conflict_record(
            remote_record.locator,
            local_record->version_vector,
            remote_record.version_vector,
            local_record->payload,
            remote_record.payload,
            ConflictClass::kStaleReplay,
            detected_at_ms,
            remote_record.correlation_id);
        return outcome;
    }

    outcome.disposition = ApplyDisposition::kConflictPersisted;
    outcome.should_apply = false;
    outcome.should_persist_conflict = true;
    outcome.checkpoint_safe = true;
    outcome.conflict_record = make_conflict_record(
        remote_record.locator,
        local_record->version_vector,
        remote_record.version_vector,
        local_record->payload,
        remote_record.payload,
        ConflictClass::kConcurrentUpdate,
        detected_at_ms,
        remote_record.correlation_id);
    return outcome;
}

ConflictRecord CloudVehicleSyncCore::make_conflict_record(const RecordLocator& locator,
                                                        const VersionVector& local_version,
                                                        const VersionVector& remote_version,
                                                        const ByteBuffer& local_payload,
                                                        const ByteBuffer& remote_payload,
                                                        ConflictClass conflict_class,
                                                        std::uint64_t detected_at_ms,
                                                        const std::string& correlation_id) {
    ConflictRecord conflict;
    conflict.locator = locator;
    conflict.local_version = local_version;
    conflict.remote_version = remote_version;
    conflict.local_payload = local_payload;
    conflict.remote_payload = remote_payload;
    conflict.conflict_class = conflict_class;
    conflict.detected_at_ms = detected_at_ms;
    conflict.correlation_id = correlation_id;
    conflict.resolved = false;
    return conflict;
}

bool CloudVehicleSyncCore::is_tombstone(const CanonicalRecord& record) {
    return record.operation == RecordOperation::kDelete;
}

CheckpointToken CloudVehicleSyncCore::advance_checkpoint_monotonic(const CheckpointToken& current,
                                                                 const RecordLocator& last_record,
                                                                 const VersionVector& last_version,
                                                                 bool should_advance) {
    if (!should_advance) {
        return current;
    }

    CheckpointToken next = current;
    next.sequence_number = current.sequence_number + 1;
    next.last_record = last_record;
    next.last_version = last_version;
    return next;
}

AckProcessingResult CloudVehicleSyncCore::process_acks(const std::vector<VersionAck>& incoming_acks,
                                                     const std::vector<VersionAck>& known_acks,
                                                     const CheckpointToken& current_checkpoint) {
    AckProcessingResult result;
    result.next_checkpoint = current_checkpoint;

    for (const VersionAck& incoming_ack : incoming_acks) {
        if (has_ack(known_acks, incoming_ack) || has_ack(result.accepted_acks, incoming_ack)) {
            result.replayed_acks.push_back(incoming_ack);
            continue;
        }

        result.accepted_acks.push_back(incoming_ack);
    }

    if (result.accepted_acks.empty()) {
        result.checkpoint_advanced = false;
        return result;
    }

    const VersionAck& last_ack = result.accepted_acks.back();
    result.next_checkpoint = advance_checkpoint_monotonic(current_checkpoint,
                                                          last_ack.locator,
                                                          last_ack.version_vector,
                                                          true);
    result.checkpoint_advanced = true;
    return result;
}

GapRecoveryDecision CloudVehicleSyncCore::decide_gap_recovery(std::uint64_t local_checksum,
                                                            std::uint64_t remote_checksum,
                                                            bool has_dirty_records) {
    GapRecoveryDecision decision;
    if (local_checksum == remote_checksum) {
        decision.trigger_gap_recovery = false;
        decision.reason = "checksums_match";
        return decision;
    }

    if (has_dirty_records) {
        decision.trigger_gap_recovery = false;
        decision.reason = "checksum_mismatch_with_dirty_records";
        return decision;
    }

    decision.trigger_gap_recovery = true;
    decision.reason = "checksum_mismatch_no_dirty_records";
    return decision;
}

}
}
