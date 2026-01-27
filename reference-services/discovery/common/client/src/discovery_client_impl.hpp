#pragma once

#include <ifex/discovery.hpp>
#include <ifex/parser.hpp>
#include <grpcpp/grpcpp.h>
#include <mutex>

// Forward declare protobuf types to avoid circular dependency
namespace swdv { namespace service_discovery {
    class service_info_t;
    class get_service_service;
    class register_service_service;
    class unregister_service_service;
    class query_services_service;
    class heartbeat_service;
    class validate_method_call_service;
    class get_method_schema_service;
    enum parameter_type_t : int;
}}

namespace ifex {

class DiscoveryClientImpl : public DiscoveryClient {
public:
    explicit DiscoveryClientImpl(const std::string& discovery_endpoint);
    ~DiscoveryClientImpl() override = default;
    
    std::string register_service(
        const ServiceEndpoint& endpoint,
        const std::string& ifex_schema) override;
    
    bool unregister_service(const std::string& registration_id) override;
    
    bool send_heartbeat(
        const std::string& registration_id,
        ServiceStatus status = ServiceStatus::AVAILABLE) override;
    
    std::optional<ServiceInfo> get_service(const std::string& service_name) override;
    std::vector<ServiceInfo> query_services(const ServiceFilter& filter = {}) override;
    
    void register_status_callback(StatusCallback callback) override;

private:
    // Helper methods
    ServiceInfo convert_proto_to_service_info(const swdv::service_discovery::service_info_t& proto_info);
    swdv::service_discovery::parameter_type_t convert_type_to_proto(Type type);
    
    // gRPC connection
    std::string discovery_endpoint_;
    std::shared_ptr<grpc::Channel> channel_;
    
    // Service stubs - use void* to avoid including protobuf headers
    struct Stubs;
    std::unique_ptr<Stubs> stubs_;
    
    // Callbacks
    std::mutex callback_mutex_;
    StatusCallback status_callback_;
};

} // namespace ifex