#include "cloud_backend_transport_client.hpp"

#include <glog/logging.h>

namespace ifex::cloud {

CloudBackendTransportClient::CloudBackendTransportClient(const std::string& server_address)
    : channel_(grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials())) {

    send_stub_ = swdv::cloud_backend_transport_service::send_to_vehicle_service::NewStub(channel_);
    status_stub_ = swdv::cloud_backend_transport_service::get_vehicle_status_service::NewStub(channel_);
    channel_stub_ = swdv::cloud_backend_transport_service::get_channel_info_service::NewStub(channel_);
    queue_stub_ = swdv::cloud_backend_transport_service::get_queue_status_service::NewStub(channel_);
    stats_stub_ = swdv::cloud_backend_transport_service::get_stats_service::NewStub(channel_);
    health_stub_ = swdv::cloud_backend_transport_service::healthy_service::NewStub(channel_);
    msg_event_stub_ = swdv::cloud_backend_transport_service::on_vehicle_message_service::NewStub(channel_);
    ack_event_stub_ = swdv::cloud_backend_transport_service::on_ack_service::NewStub(channel_);
    status_event_stub_ = swdv::cloud_backend_transport_service::on_vehicle_status_service::NewStub(channel_);
    queue_event_stub_ = swdv::cloud_backend_transport_service::on_queue_status_changed_service::NewStub(channel_);
}

CloudBackendTransportClient::~CloudBackendTransportClient() {
    StopSubscriptions();
}

// =============================================================================
// Methods
// =============================================================================

swdv::cloud_backend_transport_service::send_response_t CloudBackendTransportClient::SendToVehicle(
    const std::string& vehicle_id,
    const std::vector<uint8_t>& payload,
    swdv::cloud_backend_transport_service::persistence_t persistence) {

    grpc::ClientContext context;
    swdv::cloud_backend_transport_service::send_to_vehicle_request request;
    swdv::cloud_backend_transport_service::send_to_vehicle_response response;

    auto* req = request.mutable_request();
    req->set_vehicle_id(vehicle_id);
    req->set_payload(payload.data(), payload.size());
    req->set_persistence(persistence);

    grpc::Status status = send_stub_->send_to_vehicle(&context, request, &response);

    if (!status.ok()) {
        LOG(ERROR) << "send_to_vehicle failed: " << status.error_message();
        swdv::cloud_backend_transport_service::send_response_t result;
        result.set_status(swdv::cloud_backend_transport_service::publish_status_t::INVALID_REQUEST);
        return result;
    }

    return response.result();
}

std::pair<swdv::cloud_backend_transport_service::vehicle_status_t, int64_t>
CloudBackendTransportClient::GetVehicleStatus(const std::string& vehicle_id) {

    grpc::ClientContext context;
    swdv::cloud_backend_transport_service::get_vehicle_status_request request;
    swdv::cloud_backend_transport_service::get_vehicle_status_response response;

    request.set_vehicle_id(vehicle_id);

    grpc::Status status = status_stub_->get_vehicle_status(&context, request, &response);

    if (!status.ok()) {
        LOG(ERROR) << "get_vehicle_status failed: " << status.error_message();
        return {swdv::cloud_backend_transport_service::vehicle_status_t::UNKNOWN, 0};
    }

    return {response.status(), response.last_seen_ms()};
}

swdv::cloud_backend_transport_service::channel_info_t CloudBackendTransportClient::GetChannelInfo() {

    grpc::ClientContext context;
    swdv::cloud_backend_transport_service::get_channel_info_request request;
    swdv::cloud_backend_transport_service::get_channel_info_response response;

    grpc::Status status = channel_stub_->get_channel_info(&context, request, &response);

    if (!status.ok()) {
        LOG(ERROR) << "get_channel_info failed: " << status.error_message();
        return {};
    }

    return response.info();
}

swdv::cloud_backend_transport_service::queue_status_t
CloudBackendTransportClient::GetQueueStatus(const std::string& vehicle_id) {

    grpc::ClientContext context;
    swdv::cloud_backend_transport_service::get_queue_status_request request;
    swdv::cloud_backend_transport_service::get_queue_status_response response;

    request.set_vehicle_id(vehicle_id);

    grpc::Status status = queue_stub_->get_queue_status(&context, request, &response);

    if (!status.ok()) {
        LOG(ERROR) << "get_queue_status failed: " << status.error_message();
        return {};
    }

    return response.status();
}

swdv::cloud_backend_transport_service::transport_stats_t CloudBackendTransportClient::GetStats() {

    grpc::ClientContext context;
    swdv::cloud_backend_transport_service::get_stats_request request;
    swdv::cloud_backend_transport_service::get_stats_response response;

    grpc::Status status = stats_stub_->get_stats(&context, request, &response);

    if (!status.ok()) {
        LOG(ERROR) << "get_stats failed: " << status.error_message();
        return {};
    }

    return response.stats();
}

bool CloudBackendTransportClient::IsHealthy() {

    grpc::ClientContext context;
    swdv::cloud_backend_transport_service::healthy_request request;
    swdv::cloud_backend_transport_service::healthy_response response;

    grpc::Status status = health_stub_->healthy(&context, request, &response);

    if (!status.ok()) {
        return false;
    }

    return response.is_healthy();
}

// =============================================================================
// Event Subscriptions
// =============================================================================

void CloudBackendTransportClient::SubscribeToVehicleMessages(VehicleMessageCallback callback) {
    if (message_thread_.joinable()) {
        LOG(WARNING) << "Already subscribed to vehicle messages";
        return;
    }

    message_thread_ = std::thread([this, callback = std::move(callback)]() {
        while (running_.load()) {
            {
                std::lock_guard lock(context_mutex_);
                message_context_ = std::make_unique<grpc::ClientContext>();
            }

            swdv::cloud_backend_transport_service::on_vehicle_message_subscribe_request request;
            auto reader = msg_event_stub_->subscribe(message_context_.get(), request);

            swdv::cloud_backend_transport_service::on_vehicle_message msg;
            while (reader->Read(&msg)) {
                const auto& vm = msg.message();
                std::vector<uint8_t> payload(vm.payload().begin(), vm.payload().end());
                callback(vm.vehicle_id(), payload, vm.sequence(), vm.timestamp_ms());
            }

            grpc::Status status = reader->Finish();
            if (!running_.load()) {
                break;
            }
            if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED) {
                LOG(WARNING) << "Vehicle message stream ended: " << status.error_message();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
}

void CloudBackendTransportClient::SubscribeToVehicleStatus(VehicleStatusCallback callback) {
    if (status_thread_.joinable()) {
        LOG(WARNING) << "Already subscribed to vehicle status";
        return;
    }

    status_thread_ = std::thread([this, callback = std::move(callback)]() {
        while (running_.load()) {
            {
                std::lock_guard lock(context_mutex_);
                status_context_ = std::make_unique<grpc::ClientContext>();
            }

            swdv::cloud_backend_transport_service::on_vehicle_status_subscribe_request request;
            auto reader = status_event_stub_->subscribe(status_context_.get(), request);

            swdv::cloud_backend_transport_service::on_vehicle_status msg;
            while (reader->Read(&msg)) {
                const auto& evt = msg.event();
                callback(evt.vehicle_id(), evt.status(), evt.timestamp_ms());
            }

            grpc::Status status = reader->Finish();
            if (!running_.load()) {
                break;
            }
            if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED) {
                LOG(WARNING) << "Vehicle status stream ended: " << status.error_message();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
}

void CloudBackendTransportClient::SubscribeToAcks(AckCallback callback) {
    if (ack_thread_.joinable()) {
        LOG(WARNING) << "Already subscribed to acks";
        return;
    }

    ack_thread_ = std::thread([this, callback = std::move(callback)]() {
        while (running_.load()) {
            {
                std::lock_guard lock(context_mutex_);
                ack_context_ = std::make_unique<grpc::ClientContext>();
            }

            swdv::cloud_backend_transport_service::on_ack_subscribe_request request;
            auto reader = ack_event_stub_->subscribe(ack_context_.get(), request);

            swdv::cloud_backend_transport_service::on_ack msg;
            while (reader->Read(&msg)) {
                const auto& ack = msg.ack();
                callback(ack.vehicle_id(), ack.sequence());
            }

            grpc::Status status = reader->Finish();
            if (!running_.load()) {
                break;
            }
            if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED) {
                LOG(WARNING) << "Ack stream ended: " << status.error_message();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
}

void CloudBackendTransportClient::SubscribeToQueueStatus(QueueStatusCallback callback) {
    if (queue_thread_.joinable()) {
        LOG(WARNING) << "Already subscribed to queue status";
        return;
    }

    queue_thread_ = std::thread([this, callback = std::move(callback)]() {
        while (running_.load()) {
            {
                std::lock_guard lock(context_mutex_);
                queue_context_ = std::make_unique<grpc::ClientContext>();
            }

            swdv::cloud_backend_transport_service::on_queue_status_changed_subscribe_request request;
            auto reader = queue_event_stub_->subscribe(queue_context_.get(), request);

            swdv::cloud_backend_transport_service::on_queue_status_changed msg;
            while (reader->Read(&msg)) {
                const auto& qs = msg.status();
                callback(qs.vehicle_id(), qs.level(), qs.queue_size(), qs.queue_capacity());
            }

            grpc::Status status = reader->Finish();
            if (!running_.load()) {
                break;
            }
            if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED) {
                LOG(WARNING) << "Queue status stream ended: " << status.error_message();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
}

void CloudBackendTransportClient::StopSubscriptions() {
    running_.store(false);

    // Cancel all contexts
    {
        std::lock_guard lock(context_mutex_);
        if (message_context_) message_context_->TryCancel();
        if (status_context_) status_context_->TryCancel();
        if (ack_context_) ack_context_->TryCancel();
        if (queue_context_) queue_context_->TryCancel();
    }

    // Join threads
    if (message_thread_.joinable()) message_thread_.join();
    if (status_thread_.joinable()) status_thread_.join();
    if (ack_thread_.joinable()) ack_thread_.join();
    if (queue_thread_.joinable()) queue_thread_.join();
}

}  // namespace ifex::cloud
