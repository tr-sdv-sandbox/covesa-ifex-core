#include "../include/cloud_vehicle_sync_bridge.hpp"

#include "../../common/include/cloud_vehicle_sync_core.hpp"
#include "../../../backend-transport/common/client/include/backend_transport_client.hpp"
#include "cloud-vehicle-sync-envelope.pb.h"
#include "../../../backend-transport/cloud/service/include/cloud_backend_transport_client.hpp"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>

namespace ifex {
namespace sync {
namespace bridge {
namespace {

using ProtoEnvelope = swdv::cloud_vehicle_sync_envelope::CloudVehicleSyncEnvelope;
using ProtoSyncExchange = swdv::cloud_vehicle_sync_envelope::SyncExchange;
using ProtoCheckpointAdvance = swdv::cloud_vehicle_sync_envelope::CheckpointAdvance;
using ProtoGapRecoveryRequest = swdv::cloud_vehicle_sync_envelope::GapRecoveryRequest;
using ProtoGapRecoveryResponse = swdv::cloud_vehicle_sync_envelope::GapRecoveryResponse;
using ProtoRecordEnvelope = swdv::cloud_vehicle_sync_envelope::RecordEnvelope;
using ProtoVersionAck = swdv::cloud_vehicle_sync_envelope::VersionAck;
using ProtoRecordLocator = swdv::cloud_vehicle_sync_envelope::RecordLocator;
using ProtoVersionVector = swdv::cloud_vehicle_sync_envelope::VersionVector;

using Clock = std::chrono::steady_clock;

std::string bytes_to_hex(const ByteBuffer& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2U);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2U] = kHex[(bytes[i] >> 4U) & 0x0FU];
        out[i * 2U + 1U] = kHex[bytes[i] & 0x0FU];
    }
    return out;
}

std::string locator_key(const RecordLocator& locator) {
    return locator.namespace_name + "|" + locator.origin_node_id + "|" + bytes_to_hex(locator.record_id);
}

std::string ack_key(const VersionAck& ack) {
    const auto& v = ack.version_vector;
    std::ostringstream ss;
    ss << locator_key(ack.locator) << "|" << v.cloud_seq << "|" << v.truck_seq;
    return ss.str();
}

std::vector<VersionAck> merge_known_acks(const std::vector<VersionAck>& durable_acks,
                                         const std::vector<VersionAck>& in_memory_acks) {
    std::vector<VersionAck> merged;
    merged.reserve(durable_acks.size() + in_memory_acks.size());
    std::unordered_set<std::string> seen_keys;
    seen_keys.reserve(durable_acks.size() + in_memory_acks.size());

    for (const VersionAck& ack : durable_acks) {
        const std::string key = ack_key(ack);
        if (seen_keys.insert(key).second) {
            merged.push_back(ack);
        }
    }
    for (const VersionAck& ack : in_memory_acks) {
        const std::string key = ack_key(ack);
        if (seen_keys.insert(key).second) {
            merged.push_back(ack);
        }
    }

    return merged;
}

bool sender_matches_expected(const std::string& sender_node_id,
                             const std::string& expected_remote_node_id) {
    return !sender_node_id.empty() && sender_node_id == expected_remote_node_id;
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

RecordOperation from_proto_operation(swdv::cloud_vehicle_sync_envelope::RecordOperation op) {
    switch (op) {
        case swdv::cloud_vehicle_sync_envelope::RECORD_OPERATION_CREATE:
            return RecordOperation::kCreate;
        case swdv::cloud_vehicle_sync_envelope::RECORD_OPERATION_UPDATE:
            return RecordOperation::kUpdate;
        case swdv::cloud_vehicle_sync_envelope::RECORD_OPERATION_DELETE:
            return RecordOperation::kDelete;
    }
    return RecordOperation::kCreate;
}

swdv::cloud_vehicle_sync_envelope::RecordOperation to_proto_operation(RecordOperation op) {
    switch (op) {
        case RecordOperation::kCreate:
            return swdv::cloud_vehicle_sync_envelope::RECORD_OPERATION_CREATE;
        case RecordOperation::kUpdate:
            return swdv::cloud_vehicle_sync_envelope::RECORD_OPERATION_UPDATE;
        case RecordOperation::kDelete:
            return swdv::cloud_vehicle_sync_envelope::RECORD_OPERATION_DELETE;
    }
    return swdv::cloud_vehicle_sync_envelope::RECORD_OPERATION_CREATE;
}

VersionVector from_proto_version(const ProtoVersionVector& proto) {
    VersionVector version;
    version.cloud_seq = proto.cloud_seq();
    version.truck_seq = proto.truck_seq();
    return version;
}

void to_proto_version(const VersionVector& version, ProtoVersionVector* proto) {
    proto->set_cloud_seq(version.cloud_seq);
    proto->set_truck_seq(version.truck_seq);
}

RecordLocator from_proto_locator(const ProtoRecordLocator& proto) {
    RecordLocator locator;
    locator.namespace_name = proto.namespace_name();
    locator.origin_node_id = proto.origin_node_id();
    locator.record_id.assign(proto.record_id().begin(), proto.record_id().end());
    return locator;
}

void to_proto_locator(const RecordLocator& locator, ProtoRecordLocator* proto) {
    proto->set_namespace_name(locator.namespace_name);
    proto->set_origin_node_id(locator.origin_node_id);
    proto->set_record_id(locator.record_id.data(), locator.record_id.size());
}

CanonicalRecord from_proto_record(const ProtoRecordEnvelope& proto) {
    CanonicalRecord record;
    record.locator = from_proto_locator(proto.locator());
    record.version_vector = from_proto_version(proto.version_vector());
    record.operation = from_proto_operation(proto.operation());
    record.payload.assign(proto.payload().begin(), proto.payload().end());
    record.schema_version = proto.schema_version();
    record.idempotency_key = proto.idempotency_key();
    record.correlation_id = proto.correlation_id();
    record.payload_checksum = proto.payload_checksum();
    record.wall_clock_ms = proto.wall_clock_ms();
    record.created_at_ms = proto.created_at_ms();
    record.updated_at_ms = proto.updated_at_ms();
    record.tombstone_at_ms = proto.tombstone_at_ms();
    record.tombstone_reason = proto.tombstone_reason();
    return record;
}

void to_proto_record(const CanonicalRecord& record, ProtoRecordEnvelope* proto) {
    to_proto_locator(record.locator, proto->mutable_locator());
    to_proto_version(record.version_vector, proto->mutable_version_vector());
    proto->set_operation(to_proto_operation(record.operation));
    proto->set_payload(record.payload.data(), record.payload.size());
    proto->set_schema_version(record.schema_version);
    proto->set_idempotency_key(record.idempotency_key);
    proto->set_correlation_id(record.correlation_id);
    proto->set_payload_checksum(record.payload_checksum);
    proto->set_wall_clock_ms(record.wall_clock_ms);
    proto->set_created_at_ms(record.created_at_ms);
    proto->set_updated_at_ms(record.updated_at_ms);
    proto->set_tombstone_at_ms(record.tombstone_at_ms);
    proto->set_tombstone_reason(record.tombstone_reason);
}

VersionAck from_proto_ack(const ProtoVersionAck& proto) {
    VersionAck ack;
    ack.locator = from_proto_locator(proto.locator());
    ack.version_vector = from_proto_version(proto.version_vector());
    ack.correlation_id = proto.correlation_id();
    ack.idempotency_key = proto.idempotency_key();
    return ack;
}

void to_proto_ack(const VersionAck& ack, ProtoVersionAck* proto) {
    to_proto_locator(ack.locator, proto->mutable_locator());
    to_proto_version(ack.version_vector, proto->mutable_version_vector());
    proto->set_correlation_id(ack.correlation_id);
    proto->set_idempotency_key(ack.idempotency_key);
}

CheckpointToken from_proto_checkpoint(const swdv::cloud_vehicle_sync_envelope::CheckpointToken& proto) {
    CheckpointToken checkpoint;
    checkpoint.sequence_number = proto.sequence_number();
    checkpoint.last_record = from_proto_locator(proto.last_record());
    checkpoint.last_version = from_proto_version(proto.last_version());
    return checkpoint;
}

void to_proto_checkpoint(const CheckpointToken& checkpoint,
                        swdv::cloud_vehicle_sync_envelope::CheckpointToken* proto) {
    proto->set_sequence_number(checkpoint.sequence_number);
    to_proto_locator(checkpoint.last_record, proto->mutable_last_record());
    to_proto_version(checkpoint.last_version, proto->mutable_last_version());
}

std::string ensure_record_idempotency_key(const CanonicalRecord& record, const std::string& local_node_id) {
    if (!record.idempotency_key.empty()) {
        return record.idempotency_key;
    }
    std::ostringstream ss;
    ss << local_node_id << ':' << locator_key(record.locator) << ':' << record.version_vector.cloud_seq
       << ':' << record.version_vector.truck_seq;
    return ss.str();
}

struct RuntimeConfig {
    CommonBridgeConfig common;
    std::function<bool(const std::vector<std::uint8_t>&)> send_payload;
    std::function<void(std::function<void(const std::vector<std::uint8_t>&)>)> start_receive;
    std::function<void()> stop_receive;
    std::function<bool()> is_healthy;
};

class BridgeRuntime {
public:
    explicit BridgeRuntime(RuntimeConfig config)
        : config_(std::move(config)) {
        force_sync_.store(true);
    }

    ~BridgeRuntime() {
        Stop();
    }

    bool Start() {
        if (running_.exchange(true)) {
            return true;
        }
        if (!config_.common.adapter) {
            running_.store(false);
            return false;
        }

        config_.start_receive([this](const std::vector<std::uint8_t>& payload) {
            HandleIncomingPayload(payload);
        });

        poll_thread_ = std::thread([this]() { PollLoop(); });
        return true;
    }

    void Stop() {
        if (!running_.exchange(false)) {
            return;
        }
        cv_.notify_all();
        if (poll_thread_.joinable()) {
            poll_thread_.join();
        }
        config_.stop_receive();
    }

    bool IsRunning() const {
        return running_.load();
    }

    void ForceSync() {
        force_sync_.store(true);
        cv_.notify_all();
    }

    BridgeStats GetStats() const {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        return stats_;
    }

private:
    SyncSessionKey session_key() const {
        SyncSessionKey key;
        key.local_node_id = config_.common.local_node_id;
        key.remote_node_id = config_.common.remote_node_id;
        key.namespace_name = config_.common.namespace_name;
        return key;
    }

    std::vector<CanonicalRecord> list_dirty_records(std::size_t limit) {
        DirtyRecordQuery query;
        query.session = session_key();
        query.limit = limit;
        query.include_tombstones = true;
        return config_.common.adapter->list_dirty_records(query);
    }

    std::vector<RecordLocator> list_record_ids() {
        RecordIdQuery query;
        query.namespace_name = config_.common.namespace_name;
        query.include_tombstones = true;
        query.limit = 0;
        return config_.common.adapter->list_record_ids(query);
    }

    std::vector<CanonicalRecord> load_records_for_ids(const std::vector<RecordLocator>& ids) {
        std::unordered_set<std::string> wanted;
        wanted.reserve(ids.size());
        for (const auto& id : ids) {
            wanted.insert(locator_key(id));
        }

        DirtyRecordQuery query;
        query.session.local_node_id = config_.common.local_node_id;
        query.session.remote_node_id = "__gap_fetch__";
        query.session.namespace_name = config_.common.namespace_name;
        query.limit = 0;
        query.include_tombstones = true;

        std::vector<CanonicalRecord> all = config_.common.adapter->list_dirty_records(query);
        std::vector<CanonicalRecord> selected;
        selected.reserve(ids.size());
        for (const auto& record : all) {
            if (wanted.find(locator_key(record.locator)) != wanted.end()) {
                selected.push_back(record);
            }
        }
        return selected;
    }

    std::uint64_t state_checksum() {
        StateScope scope;
        scope.namespace_name = config_.common.namespace_name;
        scope.include_tombstones = true;
        return config_.common.adapter->compute_state_checksum(scope);
    }

    void PollLoop() {
        const auto poll_interval = std::chrono::milliseconds(config_.common.poll_interval_ms);
        const auto heartbeat_interval = std::chrono::milliseconds(config_.common.heartbeat_interval_ms);
        auto last_heartbeat = Clock::now() - heartbeat_interval;

        while (running_.load()) {
            bool send_now = false;
            {
                std::unique_lock<std::mutex> lock(cv_mutex_);
                cv_.wait_for(lock, poll_interval, [this]() {
                    return !running_.load() || force_sync_.load();
                });
                send_now = force_sync_.exchange(false);
            }
            if (!running_.load()) {
                break;
            }

            const bool heartbeat_due = (Clock::now() - last_heartbeat) >= heartbeat_interval;
            if (send_now || heartbeat_due || has_pending_acks()) {
                SendSyncExchange(config_.common.max_batch_records);
                last_heartbeat = Clock::now();
            }
        }
    }

    bool has_pending_acks() {
        std::lock_guard<std::mutex> lock(ack_mutex_);
        return !pending_outgoing_acks_.empty();
    }

    void queue_outgoing_ack(const VersionAck& ack) {
        std::lock_guard<std::mutex> lock(ack_mutex_);
        const std::string key = ack_key(ack);
        if (pending_outgoing_ack_keys_.insert(key).second) {
            pending_outgoing_acks_.push_back(ack);
        }
    }

    std::vector<VersionAck> drain_pending_acks(std::size_t max_count) {
        std::lock_guard<std::mutex> lock(ack_mutex_);
        const std::size_t count = std::min(max_count, pending_outgoing_acks_.size());
        std::vector<VersionAck> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(pending_outgoing_acks_[i]);
            pending_outgoing_ack_keys_.erase(ack_key(pending_outgoing_acks_[i]));
        }
        pending_outgoing_acks_.erase(pending_outgoing_acks_.begin(),
                                     pending_outgoing_acks_.begin() + static_cast<std::ptrdiff_t>(count));
        return out;
    }

    bool send_envelope(const ProtoEnvelope& envelope) {
        std::string serialized;
        if (!envelope.SerializeToString(&serialized)) {
            return false;
        }
        return config_.send_payload(
            std::vector<std::uint8_t>(serialized.begin(), serialized.end()));
    }

    void SendSyncExchange(std::size_t max_records) {
        ProtoEnvelope envelope;
        ProtoSyncExchange* sync = envelope.mutable_sync_exchange();
        sync->set_sender_node_id(config_.common.local_node_id);
        sync->set_recipient_node_id(config_.common.remote_node_id);
        sync->set_state_checksum(state_checksum());
        sync->set_correlation_id(next_correlation_id());
        sync->set_idempotency_key(next_message_idempotency_key());

        const auto checkpoint = config_.common.adapter->read_checkpoint(session_key());
        if (checkpoint.found) {
            to_proto_checkpoint(checkpoint.checkpoint, sync->mutable_checkpoint());
        }

        std::vector<CanonicalRecord> dirty = list_dirty_records(max_records);
        for (auto& record : dirty) {
            record.idempotency_key = ensure_record_idempotency_key(record, config_.common.local_node_id);
            to_proto_record(record, sync->add_records());
        }

        std::vector<VersionAck> acks = drain_pending_acks(max_records);
        for (const auto& ack : acks) {
            to_proto_ack(ack, sync->add_acked_records());
        }

        if (sync->records_size() == 0 && sync->acked_records_size() == 0 && !force_sync_.load() &&
            !config_.is_healthy()) {
            return;
        }

        if (send_envelope(envelope)) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.sync_messages_sent++;
        }
    }

    void SendCheckpointAdvance(const std::vector<VersionAck>& accepted_acks,
                               const CheckpointToken& checkpoint,
                               std::uint64_t checksum) {
        if (accepted_acks.empty()) {
            return;
        }

        ProtoEnvelope envelope;
        ProtoCheckpointAdvance* advance = envelope.mutable_checkpoint_advance();
        advance->set_sender_node_id(config_.common.local_node_id);
        advance->set_recipient_node_id(config_.common.remote_node_id);
        advance->set_state_checksum(checksum);
        advance->set_correlation_id(next_correlation_id());
        advance->set_idempotency_key(next_message_idempotency_key());
        to_proto_checkpoint(checkpoint, advance->mutable_durable_checkpoint());
        for (const auto& ack : accepted_acks) {
            to_proto_ack(ack, advance->add_durable_acks());
        }

        if (send_envelope(envelope)) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.checkpoint_messages_sent++;
        }
    }

    void SendGapRequest(const std::vector<RecordLocator>& ids,
                        const std::vector<RecordLocator>& request_ids,
                        std::uint64_t local_checksum,
                        std::uint64_t remote_checksum,
                        const std::string& reason) {
        ProtoEnvelope envelope;
        ProtoGapRecoveryRequest* request = envelope.mutable_gap_recovery_request();
        request->set_sender_node_id(config_.common.local_node_id);
        request->set_recipient_node_id(config_.common.remote_node_id);
        request->set_local_state_checksum(local_checksum);
        request->set_remote_state_checksum(remote_checksum);
        request->set_reason(reason);
        request->set_correlation_id(next_correlation_id());
        for (const auto& id : ids) {
            to_proto_locator(id, request->add_record_ids());
        }
        for (const auto& id : request_ids) {
            to_proto_locator(id, request->add_requested_records());
        }

        if (send_envelope(envelope)) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.gap_requests_sent++;
        }
    }

    void SendGapResponse(const std::vector<RecordLocator>& ids,
                         const std::vector<RecordLocator>& request_ids,
                         std::uint64_t local_checksum,
                         std::uint64_t remote_checksum,
                         const std::string& correlation_id) {
        ProtoEnvelope envelope;
        ProtoGapRecoveryResponse* response = envelope.mutable_gap_recovery_response();
        response->set_sender_node_id(config_.common.local_node_id);
        response->set_recipient_node_id(config_.common.remote_node_id);
        response->set_local_state_checksum(local_checksum);
        response->set_remote_state_checksum(remote_checksum);
        response->set_correlation_id(correlation_id.empty() ? next_correlation_id() : correlation_id);
        for (const auto& id : ids) {
            to_proto_locator(id, response->add_record_ids());
        }
        for (const auto& id : request_ids) {
            to_proto_locator(id, response->add_requested_records());
        }

        if (send_envelope(envelope)) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.gap_responses_sent++;
        }
    }

    std::string next_correlation_id() {
        const std::uint64_t id = ++message_counter_;
        return config_.common.local_node_id + "-corr-" + std::to_string(id);
    }

    std::string next_message_idempotency_key() {
        const std::uint64_t id = ++message_counter_;
        return config_.common.local_node_id + "-msg-" + std::to_string(id);
    }

    void HandleIncomingPayload(const std::vector<std::uint8_t>& payload) {
        if (!running_.load()) {
            return;
        }

        ProtoEnvelope envelope;
        if (!envelope.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
            return;
        }

        switch (envelope.message_case()) {
            case ProtoEnvelope::kSyncExchange:
                HandleSyncExchange(envelope.sync_exchange());
                break;
            case ProtoEnvelope::kCheckpointAdvance:
                HandleCheckpointAdvance(envelope.checkpoint_advance());
                break;
            case ProtoEnvelope::kGapRecoveryRequest:
                HandleGapRequest(envelope.gap_recovery_request());
                break;
            case ProtoEnvelope::kGapRecoveryResponse:
                HandleGapResponse(envelope.gap_recovery_response());
                break;
            case ProtoEnvelope::MESSAGE_NOT_SET:
                break;
        }
    }

    void HandleSyncExchange(const ProtoSyncExchange& sync) {
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.sync_messages_received++;
        }

        if (!sync.recipient_node_id().empty() && sync.recipient_node_id() != config_.common.local_node_id) {
            return;
        }
        if (!sender_matches_expected(sync.sender_node_id(), config_.common.remote_node_id)) {
            return;
        }

        for (const auto& proto_record : sync.records()) {
            CanonicalRecord record = from_proto_record(proto_record);
            const std::string idempotency_key = ensure_record_idempotency_key(record, sync.sender_node_id());
            const ApplyResult result = config_.common.adapter->apply_record(
                record, idempotency_key, config_.common.remote_node_id);

            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                if (result.disposition == ApplyDisposition::kApplied) {
                    stats_.records_applied++;
                } else if (result.disposition == ApplyDisposition::kDuplicate) {
                    stats_.records_duplicated++;
                } else if (result.disposition == ApplyDisposition::kConflictPersisted ||
                           result.disposition == ApplyDisposition::kNonOwnerRejected ||
                           result.disposition == ApplyDisposition::kStaleRejected) {
                    stats_.records_conflicted++;
                }
            }

            VersionAck ack;
            ack.locator = record.locator;
            ack.version_vector = record.version_vector;
            ack.correlation_id = record.correlation_id;
            ack.idempotency_key = idempotency_key;
            queue_outgoing_ack(ack);
        }

        std::vector<VersionAck> incoming_acks;
        incoming_acks.reserve(sync.acked_records_size());
        for (const auto& proto_ack : sync.acked_records()) {
            incoming_acks.push_back(from_proto_ack(proto_ack));
        }
        ProcessIncomingAcks(incoming_acks, sync.state_checksum());

        const std::uint64_t local_checksum = state_checksum();
        const bool has_dirty = !list_dirty_records(1).empty();
        const GapRecoveryDecision decision = CloudVehicleSyncCore::decide_gap_recovery(
            local_checksum, sync.state_checksum(), has_dirty);

        if (decision.trigger_gap_recovery) {
            SendGapRequest(list_record_ids(), {}, local_checksum, sync.state_checksum(), decision.reason);
        }

        if (!incoming_acks.empty() || !sync.records().empty()) {
            ForceSync();
        }
    }

    void HandleCheckpointAdvance(const ProtoCheckpointAdvance& advance) {
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.checkpoint_messages_received++;
        }
        if (!advance.recipient_node_id().empty() &&
            advance.recipient_node_id() != config_.common.local_node_id) {
            return;
        }
        if (!sender_matches_expected(advance.sender_node_id(), config_.common.remote_node_id)) {
            return;
        }

        std::vector<VersionAck> incoming_acks;
        incoming_acks.reserve(advance.durable_acks_size());
        for (const auto& proto_ack : advance.durable_acks()) {
            incoming_acks.push_back(from_proto_ack(proto_ack));
        }
        ProcessIncomingAcks(incoming_acks, advance.state_checksum());
    }

    void ProcessIncomingAcks(const std::vector<VersionAck>& incoming_acks,
                             std::uint64_t remote_checksum) {
        if (incoming_acks.empty()) {
            return;
        }

        const CheckpointReadResult checkpoint_result = config_.common.adapter->read_checkpoint(session_key());
        const CheckpointToken current_checkpoint = checkpoint_result.found
                                                       ? checkpoint_result.checkpoint
                                                       : CheckpointToken();

        const std::vector<VersionAck> durable_known_acks =
            config_.common.adapter->list_remote_acks(session_key());

        std::vector<VersionAck> known_acks_copy;
        {
            std::lock_guard<std::mutex> lock(known_acks_mutex_);
            known_acks_copy = known_acks_;
        }

        const std::vector<VersionAck> known_acks =
            merge_known_acks(durable_known_acks, known_acks_copy);

        const AckProcessingResult processing = CloudVehicleSyncCore::process_acks(
            incoming_acks, known_acks, current_checkpoint);

        if (!processing.accepted_acks.empty()) {
            config_.common.adapter->persist_remote_acks(session_key(), processing.accepted_acks);
            {
                std::lock_guard<std::mutex> lock(known_acks_mutex_);
                known_acks_.insert(known_acks_.end(), processing.accepted_acks.begin(),
                                   processing.accepted_acks.end());
            }
            if (processing.checkpoint_advanced) {
                config_.common.adapter->write_checkpoint(session_key(), processing.next_checkpoint);
            }
            SendCheckpointAdvance(processing.accepted_acks, processing.next_checkpoint, state_checksum());
        }

        const std::uint64_t local_checksum = state_checksum();
        const bool has_dirty = !list_dirty_records(1).empty();
        const GapRecoveryDecision decision = CloudVehicleSyncCore::decide_gap_recovery(
            local_checksum, remote_checksum, has_dirty);
        if (decision.trigger_gap_recovery) {
            SendGapRequest(list_record_ids(), {}, local_checksum, remote_checksum, decision.reason);
        }
    }

    void HandleGapRequest(const ProtoGapRecoveryRequest& request) {
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.gap_requests_received++;
        }

        if (!request.recipient_node_id().empty() &&
            request.recipient_node_id() != config_.common.local_node_id) {
            return;
        }
        if (!sender_matches_expected(request.sender_node_id(), config_.common.remote_node_id)) {
            return;
        }

        std::vector<RecordLocator> remote_ids;
        remote_ids.reserve(request.record_ids_size());
        for (const auto& proto_id : request.record_ids()) {
            remote_ids.push_back(from_proto_locator(proto_id));
        }

        std::vector<RecordLocator> our_ids = list_record_ids();
        std::set<std::string> our_set;
        std::set<std::string> remote_set;
        std::map<std::string, RecordLocator> our_map;
        std::map<std::string, RecordLocator> remote_map;

        for (const auto& id : our_ids) {
            const std::string key = locator_key(id);
            our_set.insert(key);
            our_map.emplace(key, id);
        }
        for (const auto& id : remote_ids) {
            const std::string key = locator_key(id);
            remote_set.insert(key);
            remote_map.emplace(key, id);
        }

        std::vector<RecordLocator> request_from_remote;
        for (const auto& key : remote_set) {
            if (our_set.find(key) == our_set.end()) {
                request_from_remote.push_back(remote_map.at(key));
            }
        }

        std::vector<RecordLocator> should_send;
        for (const auto& key : our_set) {
            if (remote_set.find(key) == remote_set.end()) {
                should_send.push_back(our_map.at(key));
            }
        }

        for (const auto& requested_proto : request.requested_records()) {
            const RecordLocator requested = from_proto_locator(requested_proto);
            const std::string key = locator_key(requested);
            if (our_set.find(key) != our_set.end()) {
                should_send.push_back(requested);
            }
        }

        SendGapResponse(our_ids,
                        request_from_remote,
                        state_checksum(),
                        request.local_state_checksum(),
                        request.correlation_id());

        if (!should_send.empty()) {
            SendSelectedRecords(should_send);
        }
    }

    void HandleGapResponse(const ProtoGapRecoveryResponse& response) {
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.gap_responses_received++;
        }

        if (!response.recipient_node_id().empty() &&
            response.recipient_node_id() != config_.common.local_node_id) {
            return;
        }
        if (!sender_matches_expected(response.sender_node_id(), config_.common.remote_node_id)) {
            return;
        }

        std::vector<RecordLocator> remote_ids;
        remote_ids.reserve(response.record_ids_size());
        for (const auto& proto_id : response.record_ids()) {
            remote_ids.push_back(from_proto_locator(proto_id));
        }

        std::vector<RecordLocator> our_ids = list_record_ids();
        std::set<std::string> our_set;
        std::set<std::string> remote_set;
        std::map<std::string, RecordLocator> remote_map;
        for (const auto& id : our_ids) {
            our_set.insert(locator_key(id));
        }
        for (const auto& id : remote_ids) {
            const std::string key = locator_key(id);
            remote_set.insert(key);
            remote_map.emplace(key, id);
        }

        std::vector<RecordLocator> request_from_remote;
        for (const auto& key : remote_set) {
            if (our_set.find(key) == our_set.end()) {
                request_from_remote.push_back(remote_map.at(key));
            }
        }

        if (!request_from_remote.empty()) {
            SendGapRequest(our_ids,
                           request_from_remote,
                           state_checksum(),
                           response.local_state_checksum(),
                           "gap_response_missing_ids");
        }

        std::vector<RecordLocator> requested_by_remote;
        requested_by_remote.reserve(response.requested_records_size());
        for (const auto& proto_req : response.requested_records()) {
            requested_by_remote.push_back(from_proto_locator(proto_req));
        }
        if (!requested_by_remote.empty()) {
            SendSelectedRecords(requested_by_remote);
        }
    }

    void SendSelectedRecords(const std::vector<RecordLocator>& ids) {
        std::vector<CanonicalRecord> records = load_records_for_ids(ids);
        if (records.empty()) {
            return;
        }

        ProtoEnvelope envelope;
        ProtoSyncExchange* sync = envelope.mutable_sync_exchange();
        sync->set_sender_node_id(config_.common.local_node_id);
        sync->set_recipient_node_id(config_.common.remote_node_id);
        sync->set_state_checksum(state_checksum());
        sync->set_correlation_id(next_correlation_id());
        sync->set_idempotency_key(next_message_idempotency_key());

        for (auto& record : records) {
            record.idempotency_key = ensure_record_idempotency_key(record, config_.common.local_node_id);
            to_proto_record(record, sync->add_records());
        }

        if (send_envelope(envelope)) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.sync_messages_sent++;
        }
    }

    RuntimeConfig config_;
    std::atomic<bool> running_{false};
    std::atomic<bool> force_sync_{false};
    std::atomic<std::uint64_t> message_counter_{0};

    std::thread poll_thread_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    mutable std::mutex stats_mutex_;
    BridgeStats stats_;

    std::mutex ack_mutex_;
    std::vector<VersionAck> pending_outgoing_acks_;
    std::unordered_set<std::string> pending_outgoing_ack_keys_;

    std::mutex known_acks_mutex_;
    std::vector<VersionAck> known_acks_;
};

}

class CloudVehicleCloudBridge::Impl {
public:
    explicit Impl(CloudBridgeConfig config)
        : config_(std::move(config)) {
    }

    bool Start() {
        if (runtime_) {
            return runtime_->IsRunning();
        }

        cloud_client_ = std::make_unique<ifex::cloud::CloudBackendTransportClient>(
            config_.cloud_transport_address, config_.common.content_id);

        RuntimeConfig runtime_config;
        runtime_config.common = config_.common;
        runtime_config.send_payload = [this](const std::vector<std::uint8_t>& payload) {
            const auto result = cloud_client_->SendToVehicle(
                config_.vehicle_id,
                payload,
                swdv::cloud_backend_transport_service::persistence_t::VOLATILE);
            return static_cast<int>(result.status()) == 0;
        };
        runtime_config.start_receive = [this](std::function<void(const std::vector<std::uint8_t>&)> callback) {
            cloud_client_->SubscribeToVehicleMessages(
                [this, callback = std::move(callback)](
                    const std::string& vehicle_id,
                    const std::vector<std::uint8_t>& payload,
                    std::uint64_t,
                    std::int64_t) {
                    if (vehicle_id == config_.vehicle_id) {
                        callback(payload);
                    }
                });
        };
        runtime_config.stop_receive = [this]() {
            if (cloud_client_) {
                cloud_client_->StopSubscriptions();
            }
        };
        runtime_config.is_healthy = [this]() {
            return cloud_client_ && cloud_client_->IsHealthy();
        };

        runtime_ = std::make_unique<BridgeRuntime>(std::move(runtime_config));
        if (!runtime_->Start()) {
            runtime_.reset();
            cloud_client_.reset();
            return false;
        }
        return true;
    }

    void Stop() {
        if (runtime_) {
            runtime_->Stop();
            runtime_.reset();
        }
        if (cloud_client_) {
            cloud_client_->StopSubscriptions();
            cloud_client_.reset();
        }
    }

    bool IsRunning() const {
        return runtime_ && runtime_->IsRunning();
    }

    void ForceSync() {
        if (runtime_) {
            runtime_->ForceSync();
        }
    }

    BridgeStats GetStats() const {
        if (!runtime_) {
            return {};
        }
        return runtime_->GetStats();
    }

private:
    CloudBridgeConfig config_;
    std::unique_ptr<ifex::cloud::CloudBackendTransportClient> cloud_client_;
    std::unique_ptr<BridgeRuntime> runtime_;
};

class CloudVehicleTruckBridge::Impl {
public:
    explicit Impl(TruckBridgeConfig config)
        : config_(std::move(config)) {
    }

    bool Start() {
        if (runtime_) {
            return runtime_->IsRunning();
        }

        auto channel = grpc::CreateChannel(config_.backend_transport_address,
                                           grpc::InsecureChannelCredentials());
        truck_client_ = std::make_unique<ifex::client::BackendTransportClient>(
            channel, config_.common.content_id);

        RuntimeConfig runtime_config;
        runtime_config.common = config_.common;
        runtime_config.send_payload = [this](const std::vector<std::uint8_t>& payload) {
            const auto result = truck_client_->publish(payload, ifex::client::Persistence::Volatile);
            return result.ok();
        };
        runtime_config.start_receive = [this](std::function<void(const std::vector<std::uint8_t>&)> callback) {
            truck_client_->on_content([callback = std::move(callback)](const std::vector<std::uint8_t>& payload) {
                callback(payload);
            });
        };
        runtime_config.stop_receive = [this]() {
            if (truck_client_) {
                truck_client_->unsubscribe_all();
            }
        };
        runtime_config.is_healthy = [this]() {
            return truck_client_ && truck_client_->healthy();
        };

        runtime_ = std::make_unique<BridgeRuntime>(std::move(runtime_config));
        if (!runtime_->Start()) {
            runtime_.reset();
            truck_client_.reset();
            return false;
        }
        return true;
    }

    void Stop() {
        if (runtime_) {
            runtime_->Stop();
            runtime_.reset();
        }
        if (truck_client_) {
            truck_client_->unsubscribe_all();
            truck_client_.reset();
        }
    }

    bool IsRunning() const {
        return runtime_ && runtime_->IsRunning();
    }

    void ForceSync() {
        if (runtime_) {
            runtime_->ForceSync();
        }
    }

    BridgeStats GetStats() const {
        if (!runtime_) {
            return {};
        }
        return runtime_->GetStats();
    }

private:
    TruckBridgeConfig config_;
    std::unique_ptr<ifex::client::BackendTransportClient> truck_client_;
    std::unique_ptr<BridgeRuntime> runtime_;
};

CloudVehicleCloudBridge::CloudVehicleCloudBridge(CloudBridgeConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
}

CloudVehicleCloudBridge::~CloudVehicleCloudBridge() = default;

bool CloudVehicleCloudBridge::Start() {
    return impl_->Start();
}

void CloudVehicleCloudBridge::Stop() {
    impl_->Stop();
}

bool CloudVehicleCloudBridge::IsRunning() const {
    return impl_->IsRunning();
}

void CloudVehicleCloudBridge::ForceSync() {
    impl_->ForceSync();
}

BridgeStats CloudVehicleCloudBridge::GetStats() const {
    return impl_->GetStats();
}

CloudVehicleTruckBridge::CloudVehicleTruckBridge(TruckBridgeConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
}

CloudVehicleTruckBridge::~CloudVehicleTruckBridge() = default;

bool CloudVehicleTruckBridge::Start() {
    return impl_->Start();
}

void CloudVehicleTruckBridge::Stop() {
    impl_->Stop();
}

bool CloudVehicleTruckBridge::IsRunning() const {
    return impl_->IsRunning();
}

void CloudVehicleTruckBridge::ForceSync() {
    impl_->ForceSync();
}

BridgeStats CloudVehicleTruckBridge::GetStats() const {
    return impl_->GetStats();
}

}
}
}
