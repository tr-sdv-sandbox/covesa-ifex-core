/**
 * @file backend_transport_client.cpp
 * @brief Implementation of Backend Transport Client
 */

#include "backend_transport_client.hpp"
#include "backend-transport-service.grpc.pb.h"

#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace ifex::client {

namespace pb = swdv::backend_transport_service;

// =============================================================================
// Helper conversions
// =============================================================================

namespace {

pb::persistence_t to_proto(Persistence p) {
    switch (p) {
        case Persistence::BestEffort: return pb::BEST_EFFORT;
        case Persistence::Volatile: return pb::VOLATILE;
        case Persistence::Durable: return pb::DURABLE;
    }
    return pb::BEST_EFFORT;
}

PublishStatus from_proto(pb::publish_status_t s) {
    switch (s) {
        case pb::OK: return PublishStatus::Ok;
        case pb::QUEUE_FULL: return PublishStatus::QueueFull;
        case pb::MESSAGE_TOO_LONG: return PublishStatus::MessageTooLong;
        case pb::INVALID_REQUEST: return PublishStatus::InvalidRequest;
        default: return PublishStatus::InvalidRequest;
    }
}

QueueLevel from_proto(pb::queue_level_t l) {
    switch (l) {
        case pb::EMPTY: return QueueLevel::Empty;
        case pb::LOW: return QueueLevel::Low;
        case pb::NORMAL: return QueueLevel::Normal;
        case pb::HIGH: return QueueLevel::High;
        case pb::CRITICAL: return QueueLevel::Critical;
        case pb::FULL: return QueueLevel::Full;
        default: return QueueLevel::Empty;
    }
}

ConnectionState from_proto(pb::connection_state_t s) {
    switch (s) {
        case pb::CONNECTED: return ConnectionState::Connected;
        case pb::DISCONNECTED: return ConnectionState::Disconnected;
        case pb::CONNECTING: return ConnectionState::Connecting;
        case pb::RECONNECTING: return ConnectionState::Reconnecting;
        default: return ConnectionState::Unknown;
    }
}

DisconnectReason from_proto(pb::disconnect_reason_t r) {
    switch (r) {
        case pb::NONE: return DisconnectReason::None;
        case pb::REQUESTED: return DisconnectReason::Requested;
        case pb::NETWORK_ERROR: return DisconnectReason::NetworkError;
        case pb::BROKER_UNAVAILABLE: return DisconnectReason::BrokerUnavailable;
        case pb::AUTHENTICATION_FAILED: return DisconnectReason::AuthenticationFailed;
        case pb::PROTOCOL_ERROR: return DisconnectReason::ProtocolError;
        case pb::TLS_ERROR: return DisconnectReason::TlsError;
        default: return DisconnectReason::None;
    }
}

ConnectionStatus from_proto(const pb::connection_status_t& s) {
    ConnectionStatus result;
    result.state = from_proto(s.state());
    result.reason = from_proto(s.reason());
    result.timestamp_ns = s.timestamp_ns();
    return result;
}

}  // namespace

// =============================================================================
// Implementation class (PIMPL)
// =============================================================================

// Metadata key for channel-bound content_id
static constexpr const char* kContentIdMetadataKey = "x-content-id";

class BackendTransportClient::Impl {
public:
    Impl(std::shared_ptr<grpc::Channel> channel, uint32_t content_id)
        : channel_(std::move(channel))
        , content_id_(content_id)
        , publish_stub_(pb::publish_service::NewStub(channel_))
        , healthy_stub_(pb::healthy_service::NewStub(channel_))
        , connection_status_stub_(pb::get_connection_status_service::NewStub(channel_))
        , queue_status_stub_(pb::get_queue_status_service::NewStub(channel_))
        , stats_stub_(pb::get_stats_service::NewStub(channel_))
        , content_id_stub_(pb::get_content_id_service::NewStub(channel_))
        , content_stub_(pb::on_content_service::NewStub(channel_))
        , ack_stub_(pb::on_ack_service::NewStub(channel_))
        , connection_changed_stub_(pb::on_connection_changed_service::NewStub(channel_))
        , queue_status_changed_stub_(pb::on_queue_status_changed_service::NewStub(channel_)) {
    }

    // Add content_id metadata to context for channel binding
    void AddContentIdMetadata(grpc::ClientContext* context) {
        context->AddMetadata(kContentIdMetadataKey, std::to_string(content_id_));
    }

    ~Impl() {
        stop_all();
    }

    uint32_t content_id() const { return content_id_; }

    PublishResult publish(const std::vector<uint8_t>& payload, Persistence persistence) {
        grpc::ClientContext context;
        AddContentIdMetadata(&context);  // Channel binding via metadata

        pb::publish_request request;
        pb::publish_response response;

        auto* req = request.mutable_request();
        req->set_payload(payload.data(), payload.size());
        req->set_persistence(to_proto(persistence));

        auto status = publish_stub_->publish(&context, request, &response);

        PublishResult result;
        if (!status.ok()) {
            LOG(ERROR) << "Publish RPC failed: " << status.error_message();
            result.status = PublishStatus::InvalidRequest;
            return result;
        }

        result.sequence = response.result().sequence();
        result.status = from_proto(response.result().status());
        result.queue_level = from_proto(response.result().queue_level());
        return result;
    }

    void on_content(ContentCallback callback) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        // Stop existing subscription
        stop_content_subscription();

        if (!callback) return;

        content_callback_ = std::move(callback);
        content_running_ = true;
        content_thread_ = std::thread([this]() { content_subscription_loop(); });
    }

    void on_ack(AckCallback callback) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        stop_ack_subscription();

        if (!callback) return;

        ack_callback_ = std::move(callback);
        ack_running_ = true;
        ack_thread_ = std::thread([this]() { ack_subscription_loop(); });
    }

    void on_connection_changed(ConnectionCallback callback) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        stop_connection_subscription();

        if (!callback) return;

        connection_callback_ = std::move(callback);
        connection_running_ = true;
        connection_thread_ = std::thread([this]() { connection_subscription_loop(); });
    }

    void stop_all() {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        stop_content_subscription();
        stop_ack_subscription();
        stop_connection_subscription();
    }

    bool healthy() {
        grpc::ClientContext context;
        pb::healthy_request request;
        pb::healthy_response response;

        auto status = healthy_stub_->healthy(&context, request, &response);
        if (!status.ok()) {
            return false;
        }
        return response.is_healthy();
    }

    ConnectionStatus connection_status() {
        grpc::ClientContext context;
        pb::get_connection_status_request request;
        pb::get_connection_status_response response;

        auto status = connection_status_stub_->get_connection_status(&context, request, &response);
        if (!status.ok()) {
            return ConnectionStatus{ConnectionState::Unknown, DisconnectReason::NetworkError, 0};
        }
        return from_proto(response.status());
    }

    QueueStatus queue_status() {
        grpc::ClientContext context;
        pb::get_queue_status_request request;
        pb::get_queue_status_response response;

        auto status = queue_status_stub_->get_queue_status(&context, request, &response);
        if (!status.ok()) {
            return QueueStatus{};
        }

        QueueStatus result;
        result.level = from_proto(response.status().level());
        result.queue_size = response.status().queue_size();
        result.queue_capacity = response.status().queue_capacity();
        return result;
    }

    TransportStats stats() {
        grpc::ClientContext context;
        pb::get_stats_request request;
        pb::get_stats_response response;

        auto status = stats_stub_->get_stats(&context, request, &response);
        if (!status.ok()) {
            return TransportStats{};
        }

        TransportStats result;
        result.messages_sent = response.stats().messages_sent();
        result.messages_failed = response.stats().messages_failed();
        result.bytes_sent = response.stats().bytes_sent();
        result.messages_received = response.stats().messages_received();
        result.bytes_received = response.stats().bytes_received();
        result.last_send_timestamp_ns = response.stats().last_send_timestamp_ns();
        result.last_receive_timestamp_ns = response.stats().last_receive_timestamp_ns();
        return result;
    }

private:
    void content_subscription_loop() {
        while (content_running_) {
            auto context = std::make_unique<grpc::ClientContext>();
            AddContentIdMetadata(context.get());  // Channel binding via metadata
            {
                std::lock_guard<std::mutex> lock(context_mutex_);
                content_context_ = context.get();
            }

            pb::on_content_subscribe_request request;

            auto reader = content_stub_->subscribe(context.get(), request);

            pb::on_content event;
            while (content_running_ && reader->Read(&event)) {
                if (content_callback_) {
                    const auto& payload_str = event.message().payload();
                    std::vector<uint8_t> payload(payload_str.begin(), payload_str.end());
                    content_callback_(payload);
                }
            }

            {
                std::lock_guard<std::mutex> lock(context_mutex_);
                content_context_ = nullptr;
            }

            auto status = reader->Finish();
            if (!status.ok() && content_running_) {
                LOG(WARNING) << "Content stream disconnected: " << status.error_message();
                std::this_thread::sleep_for(std::chrono::seconds(1));  // Backoff before retry
            }
        }
    }

    void ack_subscription_loop() {
        while (ack_running_) {
            auto context = std::make_unique<grpc::ClientContext>();
            AddContentIdMetadata(context.get());  // Channel binding via metadata
            {
                std::lock_guard<std::mutex> lock(context_mutex_);
                ack_context_ = context.get();
            }

            pb::on_ack_subscribe_request request;

            auto reader = ack_stub_->subscribe(context.get(), request);

            pb::on_ack event;
            while (ack_running_ && reader->Read(&event)) {
                if (ack_callback_) {
                    // No content_id check needed - channel is already bound
                    ack_callback_(event.ack().sequence());
                }
            }

            {
                std::lock_guard<std::mutex> lock(context_mutex_);
                ack_context_ = nullptr;
            }

            auto status = reader->Finish();
            if (!status.ok() && ack_running_) {
                LOG(WARNING) << "Ack stream disconnected: " << status.error_message();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    void connection_subscription_loop() {
        while (connection_running_) {
            auto context = std::make_unique<grpc::ClientContext>();
            {
                std::lock_guard<std::mutex> lock(context_mutex_);
                connection_context_ = context.get();
            }

            pb::on_connection_changed_subscribe_request request;
            auto reader = connection_changed_stub_->subscribe(context.get(), request);

            pb::on_connection_changed event;
            while (connection_running_ && reader->Read(&event)) {
                if (connection_callback_) {
                    connection_callback_(from_proto(event.status()));
                }
            }

            {
                std::lock_guard<std::mutex> lock(context_mutex_);
                connection_context_ = nullptr;
            }

            auto status = reader->Finish();
            if (!status.ok() && connection_running_) {
                LOG(WARNING) << "Connection status stream disconnected: " << status.error_message();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    void stop_content_subscription() {
        content_running_ = false;
        {
            std::lock_guard<std::mutex> lock(context_mutex_);
            if (content_context_) {
                content_context_->TryCancel();
            }
        }
        if (content_thread_.joinable()) {
            content_thread_.join();
        }
        content_callback_ = nullptr;
    }

    void stop_ack_subscription() {
        ack_running_ = false;
        {
            std::lock_guard<std::mutex> lock(context_mutex_);
            if (ack_context_) {
                ack_context_->TryCancel();
            }
        }
        if (ack_thread_.joinable()) {
            ack_thread_.join();
        }
        ack_callback_ = nullptr;
    }

    void stop_connection_subscription() {
        connection_running_ = false;
        {
            std::lock_guard<std::mutex> lock(context_mutex_);
            if (connection_context_) {
                connection_context_->TryCancel();
            }
        }
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
        connection_callback_ = nullptr;
    }

    std::shared_ptr<grpc::Channel> channel_;
    uint32_t content_id_;

    // Stubs
    std::unique_ptr<pb::publish_service::Stub> publish_stub_;
    std::unique_ptr<pb::healthy_service::Stub> healthy_stub_;
    std::unique_ptr<pb::get_connection_status_service::Stub> connection_status_stub_;
    std::unique_ptr<pb::get_queue_status_service::Stub> queue_status_stub_;
    std::unique_ptr<pb::get_stats_service::Stub> stats_stub_;
    std::unique_ptr<pb::get_content_id_service::Stub> content_id_stub_;
    std::unique_ptr<pb::on_content_service::Stub> content_stub_;
    std::unique_ptr<pb::on_ack_service::Stub> ack_stub_;
    std::unique_ptr<pb::on_connection_changed_service::Stub> connection_changed_stub_;
    std::unique_ptr<pb::on_queue_status_changed_service::Stub> queue_status_changed_stub_;

    // Subscription state
    std::mutex subscriptions_mutex_;
    std::mutex context_mutex_;  // Protects context pointers

    std::atomic<bool> content_running_{false};
    std::thread content_thread_;
    ContentCallback content_callback_;
    grpc::ClientContext* content_context_{nullptr};

    std::atomic<bool> ack_running_{false};
    std::thread ack_thread_;
    AckCallback ack_callback_;
    grpc::ClientContext* ack_context_{nullptr};

    std::atomic<bool> connection_running_{false};
    std::thread connection_thread_;
    ConnectionCallback connection_callback_;
    grpc::ClientContext* connection_context_{nullptr};
};

// =============================================================================
// BackendTransportClient implementation
// =============================================================================

BackendTransportClient::BackendTransportClient(std::shared_ptr<grpc::Channel> channel, uint32_t content_id)
    : impl_(std::make_unique<Impl>(std::move(channel), content_id)) {
}

BackendTransportClient::~BackendTransportClient() = default;

BackendTransportClient::BackendTransportClient(BackendTransportClient&&) noexcept = default;
BackendTransportClient& BackendTransportClient::operator=(BackendTransportClient&&) noexcept = default;

uint32_t BackendTransportClient::content_id() const {
    return impl_->content_id();
}

PublishResult BackendTransportClient::publish(const std::vector<uint8_t>& payload, Persistence persistence) {
    return impl_->publish(payload, persistence);
}

PublishResult BackendTransportClient::publish(std::vector<uint8_t>&& payload, Persistence persistence) {
    return impl_->publish(payload, persistence);
}

void BackendTransportClient::on_content(ContentCallback callback) {
    impl_->on_content(std::move(callback));
}

void BackendTransportClient::on_ack(AckCallback callback) {
    impl_->on_ack(std::move(callback));
}

void BackendTransportClient::on_connection_changed(ConnectionCallback callback) {
    impl_->on_connection_changed(std::move(callback));
}

void BackendTransportClient::unsubscribe_all() {
    impl_->stop_all();
}

bool BackendTransportClient::healthy() {
    return impl_->healthy();
}

ConnectionStatus BackendTransportClient::connection_status() {
    return impl_->connection_status();
}

QueueStatus BackendTransportClient::queue_status() {
    return impl_->queue_status();
}

TransportStats BackendTransportClient::stats() {
    return impl_->stats();
}

}  // namespace ifex::client
