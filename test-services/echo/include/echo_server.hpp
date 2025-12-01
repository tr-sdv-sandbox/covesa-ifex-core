#pragma once

#include "echo_service.grpc.pb.h"
#include "service-discovery-service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

namespace ifex::test {

// Combined service that implements all echo service methods
class EchoServer final : 
    public swdv::echo_service::echo_service::Service,
    public swdv::echo_service::echo_with_delay_service::Service,
    public swdv::echo_service::concat_service::Service {
public:
    EchoServer();
    ~EchoServer();
    
    // Server lifecycle
    void Start(const std::string& listen_address);
    void Shutdown();
    void Wait();
    
    // echo_service methods
    grpc::Status echo(grpc::ServerContext* context,
                     const swdv::echo_service::echo_request* request,
                     swdv::echo_service::echo_response* response) override;
    
    // echo_with_delay_service methods
    grpc::Status echo_with_delay(grpc::ServerContext* context,
                                const swdv::echo_service::echo_with_delay_request* request,
                                swdv::echo_service::echo_with_delay_response* response) override;
    
    // concat_service methods
    grpc::Status concat(grpc::ServerContext* context,
                       const swdv::echo_service::concat_request* request,
                       swdv::echo_service::concat_response* response) override;
    
    // Registration with discovery
    bool RegisterWithDiscovery(const std::string& discovery_endpoint, 
                              int port, 
                              const std::string& ifex_schema);

private:
    // Server instance
    std::unique_ptr<grpc::Server> server_;
    
    // Registration ID from discovery service
    std::string registration_id_;
};

} // namespace ifex::test