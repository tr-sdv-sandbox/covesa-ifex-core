/**
 * @file cloud_dispatcher_service.cpp
 * @brief In-memory cloud dispatcher service implementation
 */

#include "cloud_dispatcher_service.hpp"
#include "cloud-backend-transport-service.grpc.pb.h"

#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace ifex::cloud {

namespace transport_pb = swdv::cloud_backend_transport_service;
namespace rpc_pb = swdv::dispatcher_rpc_envelope;

// =============================================================================
// TransportClient - wrapper for cloud backend transport
// =============================================================================

class CloudDispatcherService::TransportClient {
public:
    TransportClient(const std::string& endpoint, uint32_t content_id)
        : endpoint_(endpoint), content_id_(content_id) {
        channel_ = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
    }

    bool Connect() {
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        return channel_->WaitForConnected(deadline);
    }

    bool IsConnected() {
        auto state = channel_->GetState(false);
        return state == GRPC_CHANNEL_READY;
    }

    // Subscribe to vehicle messages
    void SubscribeToMessages(
        std::function<void(const std::string&, const std::vector<uint8_t>&)> callback,
        std::atomic<bool>& running) {

        auto stub = transport_pb::on_vehicle_message_service::NewStub(channel_);

        while (running.load()) {
            grpc::ClientContext context;
            transport_pb::on_vehicle_message_subscribe_request request;
            request.set_content_id(content_id_);  // Specify content_id for on-demand MQTT subscription

            auto reader = stub->subscribe(&context, request);
            transport_pb::on_vehicle_message msg;

            while (reader->Read(&msg) && running.load()) {
                const auto& vehicle_msg = msg.message();
                std::vector<uint8_t> payload(
                    vehicle_msg.payload().begin(),
                    vehicle_msg.payload().end());
                callback(vehicle_msg.vehicle_id(), payload);
            }

            auto status = reader->Finish();
            if (!status.ok() && running.load()) {
                LOG(WARNING) << "Message subscription ended: " << status.error_message();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    // Send message to vehicle
    bool SendToVehicle(const std::string& vehicle_id, const std::vector<uint8_t>& payload) {
        auto stub = transport_pb::send_to_vehicle_service::NewStub(channel_);

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        transport_pb::send_to_vehicle_request request;
        auto* req = request.mutable_request();
        req->set_vehicle_id(vehicle_id);
        req->set_payload(payload.data(), payload.size());
        req->set_persistence(transport_pb::VOLATILE);

        transport_pb::send_to_vehicle_response response;
        auto status = stub->send_to_vehicle(&context, request, &response);

        if (!status.ok()) {
            LOG(WARNING) << "Failed to send to vehicle " << vehicle_id
                         << ": " << status.error_message();
            return false;
        }

        return response.result().status() == transport_pb::OK;
    }

private:
    std::string endpoint_;
    uint32_t content_id_;
    std::shared_ptr<grpc::Channel> channel_;
};

// =============================================================================
// CloudDispatcherService
// =============================================================================

CloudDispatcherService::CloudDispatcherService(const Config& config)
    : config_(config) {
    LOG(INFO) << "Creating CloudDispatcherService";
    LOG(INFO) << "  Transport endpoint: " << config_.transport_endpoint;
    LOG(INFO) << "  Content ID: " << config_.content_id;
    LOG(INFO) << "  Default timeout: " << config_.default_timeout_ms << "ms";
}

CloudDispatcherService::~CloudDispatcherService() {
    Stop();
}

bool CloudDispatcherService::Start() {
    if (running_.load()) {
        LOG(WARNING) << "CloudDispatcherService already running";
        return true;
    }

    LOG(INFO) << "Starting CloudDispatcherService...";

    transport_ = std::make_unique<TransportClient>(
        config_.transport_endpoint, config_.content_id);

    if (!transport_->Connect()) {
        LOG(ERROR) << "Failed to connect to transport at " << config_.transport_endpoint;
        return false;
    }

    connected_.store(true);
    running_.store(true);

    // Start message subscription thread
    subscription_thread_ = std::thread([this]() {
        transport_->SubscribeToMessages(
            [this](const std::string& vehicle_id, const std::vector<uint8_t>& payload) {
                HandleV2cMessage(vehicle_id, payload);
            },
            running_);
    });

    // Start timeout checker thread
    timeout_thread_ = std::thread([this]() {
        TimeoutCheckerLoop();
    });

    LOG(INFO) << "CloudDispatcherService started";
    return true;
}

void CloudDispatcherService::Stop() {
    if (!running_.load()) {
        return;
    }

    LOG(INFO) << "Stopping CloudDispatcherService...";
    running_.store(false);
    connected_.store(false);

    // Wake up any waiting calls
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto& [id, call] : pending_calls_) {
            std::lock_guard<std::mutex> call_lock(call->mutex);
            call->completed = true;
            call->cv.notify_all();
        }
    }

    if (subscription_thread_.joinable()) {
        subscription_thread_.join();
    }

    if (timeout_thread_.joinable()) {
        timeout_thread_.join();
    }

    transport_.reset();
    LOG(INFO) << "CloudDispatcherService stopped";
}

int64_t CloudDispatcherService::NowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string CloudDispatcherService::GenerateCorrelationId() {
    uint64_t counter = correlation_counter_.fetch_add(1);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis;

    std::stringstream ss;
    ss << "cloud-" << std::hex << std::setw(8) << std::setfill('0') << dis(gen)
       << "-" << std::dec << counter;
    return ss.str();
}

// =============================================================================
// Message Handling
// =============================================================================

void CloudDispatcherService::HandleV2cMessage(
    const std::string& /*vehicle_id*/,
    const std::vector<uint8_t>& payload) {

    rpc_pb::rpc_response_t rpc_response;
    if (!rpc_response.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        LOG(WARNING) << "Failed to parse RPC response";
        return;
    }

    const auto& correlation_id = rpc_response.correlation_id();

    VLOG(1) << "Received RPC response: " << correlation_id
            << " status=" << static_cast<int>(rpc_response.status());

    // Find pending call
    std::shared_ptr<PendingCall> call;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_calls_.find(correlation_id);
        if (it == pending_calls_.end()) {
            LOG(WARNING) << "Received response for unknown correlation_id: " << correlation_id;
            return;
        }
        call = it->second;
        pending_calls_.erase(it);
    }

    // Fill response
    {
        std::lock_guard<std::mutex> call_lock(call->mutex);

        auto* result = &call->response;
        result->set_correlation_id(correlation_id);

        // Map RPC status to cloud dispatcher status
        switch (rpc_response.status()) {
            case rpc_pb::SUCCESS:
                result->set_status(swdv::cloud_dispatcher_service::SUCCESS);
                break;
            case rpc_pb::FAILED:
                result->set_status(swdv::cloud_dispatcher_service::FAILED);
                break;
            case rpc_pb::TIMEOUT:
                result->set_status(swdv::cloud_dispatcher_service::TIMEOUT);
                break;
            case rpc_pb::SERVICE_UNAVAILABLE:
                result->set_status(swdv::cloud_dispatcher_service::SERVICE_UNAVAILABLE);
                break;
            case rpc_pb::METHOD_NOT_FOUND:
                result->set_status(swdv::cloud_dispatcher_service::METHOD_NOT_FOUND);
                break;
            case rpc_pb::INVALID_PARAMETERS:
                result->set_status(swdv::cloud_dispatcher_service::INVALID_PARAMETERS);
                break;
            default:
                result->set_status(swdv::cloud_dispatcher_service::FAILED);
        }

        result->set_result_json(rpc_response.result_json());
        result->set_error_message(rpc_response.error_message());
        result->set_duration_ms(rpc_response.duration_ms());
        result->set_service_endpoint(rpc_response.service_endpoint());

        call->completed = true;
        call->cv.notify_all();
    }

    // Store in completed calls for async retrieval
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        completed_calls_[correlation_id] = call;
    }
}

bool CloudDispatcherService::SendRequest(
    const std::string& vehicle_id,
    const std::string& correlation_id,
    const std::string& service_name,
    const std::string& method_name,
    const std::string& parameters_json,
    uint32_t timeout_ms) {

    rpc_pb::rpc_request_t request;
    request.set_correlation_id(correlation_id);
    request.set_service_name(service_name);
    request.set_method_name(method_name);
    request.set_parameters_json(parameters_json);
    request.set_timeout_ms(timeout_ms);
    request.set_request_timestamp_ms(NowMs());

    std::string serialized;
    if (!request.SerializeToString(&serialized)) {
        LOG(ERROR) << "Failed to serialize RPC request";
        return false;
    }

    std::vector<uint8_t> payload(serialized.begin(), serialized.end());
    return transport_->SendToVehicle(vehicle_id, payload);
}

void CloudDispatcherService::TimeoutCheckerLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if (!running_.load()) break;

        auto now_ms = NowMs();
        std::vector<std::shared_ptr<PendingCall>> timed_out;

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (auto it = pending_calls_.begin(); it != pending_calls_.end();) {
                if (now_ms >= it->second->expires_at_ms) {
                    timed_out.push_back(it->second);
                    it = pending_calls_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Complete timed out calls
        for (auto& call : timed_out) {
            LOG(WARNING) << "RPC call timed out: " << call->correlation_id;

            std::lock_guard<std::mutex> call_lock(call->mutex);
            call->response.set_correlation_id(call->correlation_id);
            call->response.set_status(swdv::cloud_dispatcher_service::TIMEOUT);
            call->response.set_error_message("Request timed out");
            call->response.set_duration_ms(call->timeout_ms);
            call->completed = true;
            call->cv.notify_all();

            // Store in completed for async retrieval
            {
                std::lock_guard<std::mutex> lock(completed_mutex_);
                completed_calls_[call->correlation_id] = call;
            }
        }
    }
}

// =============================================================================
// gRPC Method Implementations
// =============================================================================

grpc::Status CloudDispatcherService::call_method(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_dispatcher_service::call_method_request* request,
    swdv::cloud_dispatcher_service::call_method_response* response) {

    const auto& req = request->request();
    const auto& vehicle_id = req.vehicle_id();
    const auto& service_name = req.service_name();
    const auto& method_name = req.method_name();
    const auto& parameters_json = req.parameters_json();
    uint32_t timeout_ms = req.timeout_ms() > 0 ? req.timeout_ms() : config_.default_timeout_ms;

    auto correlation_id = GenerateCorrelationId();

    LOG(INFO) << "call_method: " << vehicle_id << "/" << service_name << "." << method_name
              << " correlation_id=" << correlation_id;

    // Create pending call
    auto call = std::make_shared<PendingCall>();
    call->correlation_id = correlation_id;
    call->vehicle_id = vehicle_id;
    call->service_name = service_name;
    call->method_name = method_name;
    call->created_at_ms = NowMs();
    call->timeout_ms = timeout_ms;
    call->expires_at_ms = call->created_at_ms + timeout_ms;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_calls_[correlation_id] = call;
    }

    // Send request
    if (!SendRequest(vehicle_id, correlation_id, service_name, method_name,
                     parameters_json, timeout_ms)) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_calls_.erase(correlation_id);

        auto* result = response->mutable_result();
        result->set_correlation_id(correlation_id);
        result->set_status(swdv::cloud_dispatcher_service::TRANSPORT_ERROR);
        result->set_error_message("Failed to send request to vehicle");
        return grpc::Status::OK;
    }

    // Wait for response
    {
        std::unique_lock<std::mutex> lock(call->mutex);
        if (!call->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [&call]() { return call->completed; })) {
            // Timeout - remove from pending
            std::lock_guard<std::mutex> pending_lock(pending_mutex_);
            pending_calls_.erase(correlation_id);

            auto* result = response->mutable_result();
            result->set_correlation_id(correlation_id);
            result->set_status(swdv::cloud_dispatcher_service::TIMEOUT);
            result->set_error_message("Request timed out waiting for response");
            return grpc::Status::OK;
        }
    }

    // Copy response
    *response->mutable_result() = call->response;
    return grpc::Status::OK;
}

grpc::Status CloudDispatcherService::call_method_async(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_dispatcher_service::call_method_async_request* request,
    swdv::cloud_dispatcher_service::call_method_async_response* response) {

    const auto& req = request->request();
    const auto& vehicle_id = req.vehicle_id();
    const auto& service_name = req.service_name();
    const auto& method_name = req.method_name();
    const auto& parameters_json = req.parameters_json();
    uint32_t timeout_ms = req.timeout_ms() > 0 ? req.timeout_ms() : config_.default_timeout_ms;

    auto correlation_id = GenerateCorrelationId();

    LOG(INFO) << "call_method_async: " << vehicle_id << "/" << service_name << "." << method_name
              << " correlation_id=" << correlation_id;

    // Create pending call
    auto call = std::make_shared<PendingCall>();
    call->correlation_id = correlation_id;
    call->vehicle_id = vehicle_id;
    call->service_name = service_name;
    call->method_name = method_name;
    call->created_at_ms = NowMs();
    call->timeout_ms = timeout_ms;
    call->expires_at_ms = call->created_at_ms + timeout_ms;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_calls_[correlation_id] = call;
    }

    // Send request
    if (!SendRequest(vehicle_id, correlation_id, service_name, method_name,
                     parameters_json, timeout_ms)) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_calls_.erase(correlation_id);

        response->set_correlation_id(correlation_id);
        response->set_accepted(false);
        return grpc::Status::OK;
    }

    response->set_correlation_id(correlation_id);
    response->set_accepted(true);
    return grpc::Status::OK;
}

grpc::Status CloudDispatcherService::get_call_result(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_dispatcher_service::get_call_result_request* request,
    swdv::cloud_dispatcher_service::get_call_result_response* response) {

    const auto& correlation_id = request->correlation_id();

    // Check completed calls first
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        auto it = completed_calls_.find(correlation_id);
        if (it != completed_calls_.end()) {
            response->set_found(true);
            response->set_completed(true);
            *response->mutable_result() = it->second->response;
            completed_calls_.erase(it);
            return grpc::Status::OK;
        }
    }

    // Check pending calls
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_calls_.find(correlation_id);
        if (it != pending_calls_.end()) {
            response->set_found(true);
            response->set_completed(false);
            return grpc::Status::OK;
        }
    }

    response->set_found(false);
    response->set_completed(false);
    return grpc::Status::OK;
}

grpc::Status CloudDispatcherService::list_pending_calls(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_dispatcher_service::list_pending_calls_request* request,
    swdv::cloud_dispatcher_service::list_pending_calls_response* response) {

    const auto& vehicle_filter = request->vehicle_id();

    std::lock_guard<std::mutex> lock(pending_mutex_);

    for (const auto& [id, call] : pending_calls_) {
        if (!vehicle_filter.empty() && call->vehicle_id != vehicle_filter) {
            continue;
        }

        auto* info = response->add_calls();
        info->set_correlation_id(call->correlation_id);
        info->set_vehicle_id(call->vehicle_id);
        info->set_service_name(call->service_name);
        info->set_method_name(call->method_name);
        info->set_created_at_ms(call->created_at_ms);
        info->set_timeout_ms(call->timeout_ms);
        info->set_expires_at_ms(call->expires_at_ms);
    }

    return grpc::Status::OK;
}

grpc::Status CloudDispatcherService::cancel_call(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_dispatcher_service::cancel_call_request* request,
    swdv::cloud_dispatcher_service::cancel_call_response* response) {

    const auto& correlation_id = request->correlation_id();

    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = pending_calls_.find(correlation_id);

    if (it == pending_calls_.end()) {
        response->set_success(false);
        response->set_error_message("Call not found");
        return grpc::Status::OK;
    }

    // Mark as cancelled
    auto call = it->second;
    {
        std::lock_guard<std::mutex> call_lock(call->mutex);
        call->response.set_correlation_id(correlation_id);
        call->response.set_status(swdv::cloud_dispatcher_service::FAILED);
        call->response.set_error_message("Cancelled by user");
        call->completed = true;
        call->cv.notify_all();
    }

    pending_calls_.erase(it);

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CloudDispatcherService::healthy(
    grpc::ServerContext* /*context*/,
    const swdv::cloud_dispatcher_service::healthy_request* /*request*/,
    swdv::cloud_dispatcher_service::healthy_response* response) {

    response->set_is_healthy(connected_.load());
    return grpc::Status::OK;
}

}  // namespace ifex::cloud
