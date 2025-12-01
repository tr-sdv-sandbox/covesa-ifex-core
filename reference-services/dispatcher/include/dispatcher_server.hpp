#pragma once

#include "ifex-dispatcher-service.grpc.pb.h"
#include <ifex/discovery.hpp>
#include "../src/dynamic_caller.hpp"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

namespace ifex::reference {

// IFEX dispatcher service implementation
class DispatcherServer final : 
    public swdv::ifex_dispatcher::call_method_service::Service {
public:
    explicit DispatcherServer(const std::string& discovery_endpoint);
    ~DispatcherServer();
    
    // Server lifecycle
    void Start(const std::string& listen_address);
    void Shutdown();
    void Wait();
    
    // call_method_service methods
    grpc::Status call_method(grpc::ServerContext* context,
                           const swdv::ifex_dispatcher::call_method_request* request,
                           swdv::ifex_dispatcher::call_method_response* response) override;


    // Registration with discovery
    bool RegisterWithDiscovery(int port, const std::string& ifex_schema);

private:
    // Helper methods
    std::string FindServiceEndpoint(const std::string& service_name);
    std::string GetServiceSchema(const std::string& service_name);
    bool ValidateMethodParameters(const std::string& service_name, 
                                const std::string& method_name,
                                const nlohmann::json& parameters,
                                const std::string& ifex_schema,
                                std::vector<std::string>& errors);

    // Server instance
    std::unique_ptr<grpc::Server> server_;
    
    // Discovery endpoint
    std::string discovery_endpoint_;
    
    // Core library components
    std::unique_ptr<ifex::DiscoveryClient> discovery_client_;
    std::unique_ptr<ifex::DynamicCaller> dynamic_caller_;
};

} // namespace ifex::reference