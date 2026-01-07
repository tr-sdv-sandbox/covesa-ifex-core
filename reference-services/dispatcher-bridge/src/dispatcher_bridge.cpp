/**
 * @file dispatcher_bridge.cpp
 * @brief Implementation of DispatcherBridge
 */

#include "dispatcher_bridge.hpp"
#include "ifex_content_ids.hpp"

#include <backend_transport_client.hpp>

#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

// Generated proto headers
#include "dispatcher-rpc-envelope.pb.h"
#include "ifex-dispatcher-service.pb.h"
#include "ifex-dispatcher-service.grpc.pb.h"

#include <chrono>
#include <queue>
#include <condition_variable>

namespace ifex::reference {

namespace {

/// Get current time in milliseconds since epoch
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// Get monotonic time for timeouts
std::chrono::steady_clock::time_point Now() {
    return std::chrono::steady_clock::now();
}

}  // namespace

/// Wrapper for BackendTransportClient to avoid exposing it in header
class DispatcherBridge::TransportClientWrapper {
public:
    TransportClientWrapper(std::shared_ptr<grpc::Channel> channel, uint32_t content_id)
        : client_(channel, content_id) {}

    void OnContent(std::function<void(const std::vector<uint8_t>&)> callback) {
        client_.on_content(std::move(callback));
    }

    void OnConnectionChanged(std::function<void(const ifex::client::ConnectionStatus&)> callback) {
        client_.on_connection_changed(std::move(callback));
    }

    ifex::client::PublishResult Publish(const std::vector<uint8_t>& payload) {
        return client_.publish(payload, ifex::client::Persistence::Volatile);
    }

    bool Healthy() {
        return client_.healthy();
    }

    void UnsubscribeAll() {
        client_.unsubscribe_all();
    }

private:
    ifex::client::BackendTransportClient client_;
};

/// Thread-safe work queue for request processing
class WorkQueue {
public:
    using WorkItem = std::function<void()>;

    void Push(WorkItem item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    bool Pop(WorkItem& item, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || stopped_; })) {
            if (stopped_ && queue_.empty()) {
                return false;
            }
            item = std::move(queue_.front());
            queue_.pop();
            return true;
        }
        return false;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    std::queue<WorkItem> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_ = false;
};

// Static work queue (shared across all instances - could be per-instance if needed)
static WorkQueue g_work_queue;

DispatcherBridge::DispatcherBridge(const DispatcherBridgeConfig& config)
    : config_(config) {
    // Use content_id from config, defaulting to the standard constant
    if (config_.rpc_content_id == 0) {
        config_.rpc_content_id = ifex::content_id::DISPATCHER_RPC;
    }
}

DispatcherBridge::~DispatcherBridge() {
    Stop();
}

bool DispatcherBridge::Start() {
    if (running_.exchange(true)) {
        LOG(WARNING) << "DispatcherBridge already running";
        return false;
    }

    LOG(INFO) << "Starting DispatcherBridge...";
    LOG(INFO) << "  Dispatcher endpoint: " << config_.dispatcher_endpoint;
    LOG(INFO) << "  Backend Transport endpoint: " << config_.backend_transport_endpoint;
    LOG(INFO) << "  RPC content_id: " << config_.rpc_content_id;

    // Create Dispatcher channel
    dispatcher_channel_ = grpc::CreateChannel(
        config_.dispatcher_endpoint,
        grpc::InsecureChannelCredentials());

    // Wait briefly for Dispatcher to be ready
    if (!dispatcher_channel_->WaitForConnected(
            std::chrono::system_clock::now() + std::chrono::seconds(5))) {
        LOG(WARNING) << "Dispatcher not immediately available, will retry on requests";
    }

    // Create Backend Transport client
    auto transport_channel = grpc::CreateChannel(
        config_.backend_transport_endpoint,
        grpc::InsecureChannelCredentials());

    transport_ = std::make_unique<TransportClientWrapper>(
        transport_channel, config_.rpc_content_id);

    // Subscribe to incoming requests
    transport_->OnContent([this](const std::vector<uint8_t>& payload) {
        HandleIncomingRequest(payload);
    });

    // Subscribe to connection status for logging
    transport_->OnConnectionChanged([](const ifex::client::ConnectionStatus& status) {
        LOG(INFO) << "Backend Transport connection: "
                  << static_cast<int>(status.state);
    });

    // Start worker threads
    for (uint32_t i = 0; i < config_.num_workers; ++i) {
        workers_.emplace_back([this] {
            WorkQueue::WorkItem item;
            while (running_) {
                if (g_work_queue.Pop(item, std::chrono::milliseconds(100))) {
                    item();
                }
            }
        });
    }

    // Start timeout checker thread
    timeout_thread_ = std::thread([this] { TimeoutCheckerLoop(); });

    LOG(INFO) << "DispatcherBridge started with " << config_.num_workers << " workers";
    return true;
}

void DispatcherBridge::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    LOG(INFO) << "Stopping DispatcherBridge...";

    // Stop work queue
    g_work_queue.Stop();

    // Stop subscriptions
    if (transport_) {
        transport_->UnsubscribeAll();
    }

    // Wait for workers
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    // Wait for timeout checker
    if (timeout_thread_.joinable()) {
        timeout_thread_.join();
    }

    LOG(INFO) << "DispatcherBridge stopped";
}

bool DispatcherBridge::IsRunning() const {
    return running_.load();
}

bool DispatcherBridge::IsHealthy() const {
    if (!running_) return false;
    if (!transport_) return false;
    return transport_->Healthy();
}

DispatcherBridge::Stats DispatcherBridge::GetStats() const {
    Stats stats;
    stats.requests_received = requests_received_.load();
    stats.requests_completed = requests_completed_.load();
    stats.requests_failed = requests_failed_.load();
    stats.requests_timed_out = requests_timed_out_.load();
    stats.requests_rejected = requests_rejected_.load();

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        stats.pending_count = static_cast<uint32_t>(pending_requests_.size());
    }

    return stats;
}

void DispatcherBridge::HandleIncomingRequest(const std::vector<uint8_t>& payload) {
    requests_received_++;

    // Decode RPC request envelope
    swdv::dispatcher_rpc_envelope::rpc_request_t request;
    if (!request.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        LOG(ERROR) << "Failed to parse RPC request envelope";
        requests_rejected_++;
        return;
    }

    const auto& correlation_id = request.correlation_id();
    if (correlation_id.empty()) {
        LOG(ERROR) << "RPC request missing correlation_id";
        requests_rejected_++;
        return;
    }

    VLOG(1) << "Received RPC request: " << correlation_id
            << " -> " << request.service_name() << "." << request.method_name();

    // Check for duplicate correlation_id
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_requests_.count(correlation_id) > 0) {
            LOG(WARNING) << "Duplicate correlation_id: " << correlation_id;
            SendResponse(correlation_id,
                        swdv::dispatcher_rpc_envelope::DUPLICATE_REQUEST,
                        "", "Duplicate correlation_id", 0, "");
            requests_rejected_++;
            return;
        }

        // Check concurrent request limit
        if (pending_requests_.size() >= config_.max_concurrent_requests) {
            LOG(WARNING) << "Max concurrent requests exceeded";
            SendResponse(correlation_id,
                        swdv::dispatcher_rpc_envelope::TRANSPORT_ERROR,
                        "", "Too many concurrent requests", 0, "");
            requests_rejected_++;
            return;
        }
    }

    // Check for stale request (already expired based on timestamp)
    uint32_t timeout_ms = request.timeout_ms();
    if (timeout_ms == 0) {
        timeout_ms = config_.default_timeout_ms;
    }

    if (request.request_timestamp_ms() > 0) {
        int64_t age_ms = NowMs() - request.request_timestamp_ms();
        if (age_ms > static_cast<int64_t>(timeout_ms)) {
            LOG(WARNING) << "Request already expired: age=" << age_ms
                         << "ms, timeout=" << timeout_ms << "ms";
            SendResponse(correlation_id,
                        swdv::dispatcher_rpc_envelope::TIMEOUT,
                        "", "Request expired before processing", 0, "");
            requests_rejected_++;
            return;
        }
    }

    // Create pending request entry
    auto pending = std::make_shared<PendingRequest>();
    pending->correlation_id = correlation_id;
    pending->start_time = Now();
    pending->timeout_ms = timeout_ms;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_[correlation_id] = pending;
    }

    // Queue work for execution
    g_work_queue.Push([this, pending,
                       service_name = request.service_name(),
                       method_name = request.method_name(),
                       parameters_json = request.parameters_json(),
                       request_timestamp_ms = request.request_timestamp_ms()] {
        ExecuteRequest(pending, service_name, method_name,
                      parameters_json, request_timestamp_ms);
    });
}

void DispatcherBridge::ExecuteRequest(
    std::shared_ptr<PendingRequest> pending,
    const std::string& service_name,
    const std::string& method_name,
    const std::string& parameters_json,
    int64_t request_timestamp_ms) {

    // Check if already completed (timed out)
    if (pending->completed.exchange(true)) {
        return;
    }

    auto start_time = Now();

    // Calculate remaining timeout
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        start_time - pending->start_time).count();
    auto remaining_timeout_ms = static_cast<int64_t>(pending->timeout_ms) - elapsed;

    if (remaining_timeout_ms <= 0) {
        LOG(WARNING) << "Request timed out before dispatch: " << pending->correlation_id;
        SendResponse(pending->correlation_id,
                    swdv::dispatcher_rpc_envelope::TIMEOUT,
                    "", "Request timed out before dispatch", 0, "");
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_.erase(pending->correlation_id);
        }
        requests_timed_out_++;
        return;
    }

    // Call Dispatcher
    auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);

    swdv::ifex_dispatcher::call_method_request dispatcher_req;
    auto* call = dispatcher_req.mutable_call();
    call->set_service_name(service_name);
    call->set_method_name(method_name);
    call->set_parameters(parameters_json);
    call->set_timeout_ms(static_cast<uint32_t>(remaining_timeout_ms));

    swdv::ifex_dispatcher::call_method_response dispatcher_resp;

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                        std::chrono::milliseconds(remaining_timeout_ms));

    grpc::Status status = stub->call_method(&context, dispatcher_req, &dispatcher_resp);

    auto end_time = Now();
    auto duration_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

    // Build response
    uint8_t rpc_status;
    std::string result_json;
    std::string error_message;
    std::string service_endpoint;

    if (status.ok()) {
        const auto& result = dispatcher_resp.result();
        rpc_status = MapDispatcherStatus(result.status());
        result_json = result.response();
        error_message = result.error_message();
        service_endpoint = result.service_endpoint();

        if (result.status() == swdv::ifex_dispatcher::SUCCESS) {
            requests_completed_++;
        } else {
            requests_failed_++;
        }
    } else {
        // gRPC error
        if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
            rpc_status = swdv::dispatcher_rpc_envelope::TIMEOUT;
            error_message = "Dispatcher call timed out";
            requests_timed_out_++;
        } else if (status.error_code() == grpc::StatusCode::UNAVAILABLE) {
            rpc_status = swdv::dispatcher_rpc_envelope::SERVICE_UNAVAILABLE;
            error_message = "Dispatcher service unavailable";
            requests_failed_++;
        } else {
            rpc_status = swdv::dispatcher_rpc_envelope::TRANSPORT_ERROR;
            error_message = "gRPC error: " + status.error_message();
            requests_failed_++;
        }
    }

    SendResponse(pending->correlation_id, rpc_status, result_json,
                error_message, duration_ms, service_endpoint);

    // Remove from pending
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_.erase(pending->correlation_id);
    }
}

void DispatcherBridge::SendResponse(
    const std::string& correlation_id,
    uint8_t status,
    const std::string& result_json,
    const std::string& error_message,
    uint32_t duration_ms,
    const std::string& service_endpoint) {

    swdv::dispatcher_rpc_envelope::rpc_response_t response;
    response.set_correlation_id(correlation_id);
    response.set_status(static_cast<swdv::dispatcher_rpc_envelope::rpc_status_t>(status));
    response.set_result_json(result_json);
    response.set_error_message(error_message);
    response.set_duration_ms(duration_ms);
    response.set_service_endpoint(service_endpoint);
    response.set_response_timestamp_ms(NowMs());

    std::string serialized;
    if (!response.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize RPC response";
        return;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    auto result = transport_->Publish(payload);

    if (!result.ok()) {
        LOG(ERROR) << "Failed to publish RPC response: "
                   << static_cast<int>(result.status);
    } else {
        VLOG(1) << "Sent RPC response: " << correlation_id
                << " status=" << static_cast<int>(status);
    }
}

void DispatcherBridge::TimeoutCheckerLoop() {
    while (running_) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.timeout_check_interval_ms));

        if (!running_) break;

        auto now = Now();
        std::vector<std::shared_ptr<PendingRequest>> timed_out;

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (auto it = pending_requests_.begin(); it != pending_requests_.end();) {
                auto& pending = it->second;
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - pending->start_time).count();

                if (elapsed > pending->timeout_ms) {
                    if (!pending->completed.exchange(true)) {
                        timed_out.push_back(pending);
                    }
                    it = pending_requests_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Send timeout responses outside the lock
        for (const auto& pending : timed_out) {
            LOG(WARNING) << "Request timed out: " << pending->correlation_id;
            SendResponse(pending->correlation_id,
                        swdv::dispatcher_rpc_envelope::TIMEOUT,
                        "", "Request timed out", pending->timeout_ms, "");
            requests_timed_out_++;
        }
    }
}

uint8_t DispatcherBridge::MapDispatcherStatus(int dispatcher_status) {
    // Map from swdv::ifex_dispatcher::call_status_t to swdv::dispatcher_rpc_envelope::rpc_status_t
    // The values are the same for 0-5, but we map explicitly for safety
    switch (dispatcher_status) {
        case swdv::ifex_dispatcher::SUCCESS:
            return swdv::dispatcher_rpc_envelope::SUCCESS;
        case swdv::ifex_dispatcher::FAILED:
            return swdv::dispatcher_rpc_envelope::FAILED;
        case swdv::ifex_dispatcher::TIMEOUT:
            return swdv::dispatcher_rpc_envelope::TIMEOUT;
        case swdv::ifex_dispatcher::SERVICE_UNAVAILABLE:
            return swdv::dispatcher_rpc_envelope::SERVICE_UNAVAILABLE;
        case swdv::ifex_dispatcher::METHOD_NOT_FOUND:
            return swdv::dispatcher_rpc_envelope::METHOD_NOT_FOUND;
        case swdv::ifex_dispatcher::INVALID_PARAMETERS:
            return swdv::dispatcher_rpc_envelope::INVALID_PARAMETERS;
        default:
            return swdv::dispatcher_rpc_envelope::FAILED;
    }
}

}  // namespace ifex::reference
