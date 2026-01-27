#pragma once

#include "cloud-dispatcher-service.grpc.pb.h"
#include "dispatcher-rpc-envelope.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace ifex::cloud {

/// In-memory cloud dispatcher service.
///
/// Sends RPC requests to vehicles via cloud-backend-transport and handles responses.
///
/// Protocol (content_id=200):
/// - Cloud -> Vehicle: rpc_request_t with correlation_id, service_name, method_name, parameters_json
/// - Vehicle -> Cloud: rpc_response_t with correlation_id, status, result_json
class CloudDispatcherService final
    : public swdv::cloud_dispatcher_service::call_method_service::Service,
      public swdv::cloud_dispatcher_service::call_method_async_service::Service,
      public swdv::cloud_dispatcher_service::get_call_result_service::Service,
      public swdv::cloud_dispatcher_service::list_pending_calls_service::Service,
      public swdv::cloud_dispatcher_service::cancel_call_service::Service,
      public swdv::cloud_dispatcher_service::healthy_service::Service {
public:
    struct Config {
        // Cloud backend transport endpoint
        std::string transport_endpoint = "localhost:50100";

        // Content ID for dispatcher RPC (200)
        uint32_t content_id = 200;

        // Default timeout for RPC calls (ms)
        uint32_t default_timeout_ms = 30000;
    };

    explicit CloudDispatcherService(const Config& config);
    ~CloudDispatcherService();

    // Non-copyable
    CloudDispatcherService(const CloudDispatcherService&) = delete;
    CloudDispatcherService& operator=(const CloudDispatcherService&) = delete;

    /// Start the service
    bool Start();

    /// Stop the service
    void Stop();

    /// Check if connected to transport
    bool IsConnected() const { return connected_.load(); }

    // =========================================================================
    // gRPC Method Implementations
    // =========================================================================

    grpc::Status call_method(
        grpc::ServerContext* context,
        const swdv::cloud_dispatcher_service::call_method_request* request,
        swdv::cloud_dispatcher_service::call_method_response* response) override;

    grpc::Status call_method_async(
        grpc::ServerContext* context,
        const swdv::cloud_dispatcher_service::call_method_async_request* request,
        swdv::cloud_dispatcher_service::call_method_async_response* response) override;

    grpc::Status get_call_result(
        grpc::ServerContext* context,
        const swdv::cloud_dispatcher_service::get_call_result_request* request,
        swdv::cloud_dispatcher_service::get_call_result_response* response) override;

    grpc::Status list_pending_calls(
        grpc::ServerContext* context,
        const swdv::cloud_dispatcher_service::list_pending_calls_request* request,
        swdv::cloud_dispatcher_service::list_pending_calls_response* response) override;

    grpc::Status cancel_call(
        grpc::ServerContext* context,
        const swdv::cloud_dispatcher_service::cancel_call_request* request,
        swdv::cloud_dispatcher_service::cancel_call_response* response) override;

    grpc::Status healthy(
        grpc::ServerContext* context,
        const swdv::cloud_dispatcher_service::healthy_request* request,
        swdv::cloud_dispatcher_service::healthy_response* response) override;

private:
    // Handle incoming v2c response from vehicle
    void HandleV2cMessage(const std::string& vehicle_id, const std::vector<uint8_t>& payload);

    // Send RPC request to vehicle
    bool SendRequest(const std::string& vehicle_id,
                     const std::string& correlation_id,
                     const std::string& service_name,
                     const std::string& method_name,
                     const std::string& parameters_json,
                     uint32_t timeout_ms);

    // Generate unique correlation ID
    std::string GenerateCorrelationId();

    // Timeout checker thread
    void TimeoutCheckerLoop();

    // Helper to get current time in ms
    int64_t NowMs() const;

    Config config_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};

    // Transport client
    class TransportClient;
    std::unique_ptr<TransportClient> transport_;
    std::thread subscription_thread_;
    std::thread timeout_thread_;

    // Pending requests
    struct PendingCall {
        std::string correlation_id;
        std::string vehicle_id;
        std::string service_name;
        std::string method_name;
        int64_t created_at_ms = 0;
        uint32_t timeout_ms = 0;
        int64_t expires_at_ms = 0;

        // For synchronous calls - signaled when response arrives
        std::mutex mutex;
        std::condition_variable cv;
        bool completed = false;

        // Response data
        swdv::cloud_dispatcher_service::call_response_t response;
    };

    std::mutex pending_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingCall>> pending_calls_;

    // Completed calls (for async retrieval)
    std::mutex completed_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingCall>> completed_calls_;

    // Correlation ID counter
    std::atomic<uint64_t> correlation_counter_{0};
};

}  // namespace ifex::cloud
