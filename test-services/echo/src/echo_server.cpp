#include "echo_server.hpp"
#include <ifex/network.hpp>
#include <glog/logging.h>
#include <grpcpp/server_builder.h>
#include <thread>
#include <chrono>

namespace ifex::test {

EchoServer::EchoServer() {
    LOG(INFO) << "Initializing Echo Test Server";
}

EchoServer::~EchoServer() {
    if (server_) {
        Shutdown();
    }
}

void EchoServer::Start(const std::string& listen_address) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    
    // Register all service implementations
    builder.RegisterService(static_cast<swdv::echo_service::echo_service::Service*>(this));
    builder.RegisterService(static_cast<swdv::echo_service::echo_with_delay_service::Service*>(this));
    builder.RegisterService(static_cast<swdv::echo_service::concat_service::Service*>(this));
    
    server_ = builder.BuildAndStart();
    LOG(INFO) << "Echo server listening on " << listen_address;
}

void EchoServer::Shutdown() {
    if (server_) {
        LOG(INFO) << "Shutting down echo server";
        server_->Shutdown();
    }
}

void EchoServer::Wait() {
    if (server_) {
        server_->Wait();
    }
}

// echo method implementation
grpc::Status EchoServer::echo(
    grpc::ServerContext* context,
    const swdv::echo_service::echo_request* request,
    swdv::echo_service::echo_response* response) {
    
    LOG(INFO) << "Echo called with message: " << request->message();
    response->set_response(request->message());
    return grpc::Status::OK;
}

// echo_with_delay method implementation
grpc::Status EchoServer::echo_with_delay(
    grpc::ServerContext* context,
    const swdv::echo_service::echo_with_delay_request* request,
    swdv::echo_service::echo_with_delay_response* response) {
    
    LOG(INFO) << "Echo with delay called with message: " << request->message() 
              << ", delay: " << request->delay_ms() << "ms";
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Sleep for the requested duration
    std::this_thread::sleep_for(std::chrono::milliseconds(request->delay_ms()));
    
    auto end = std::chrono::high_resolution_clock::now();
    auto actual_delay = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    response->set_response(request->message());
    response->set_actual_delay_ms(static_cast<uint32_t>(actual_delay));
    
    return grpc::Status::OK;
}

// concat method implementation
grpc::Status EchoServer::concat(
    grpc::ServerContext* context,
    const swdv::echo_service::concat_request* request,
    swdv::echo_service::concat_response* response) {
    
    LOG(INFO) << "Concat called with first: " << request->first() 
              << ", second: " << request->second()
              << ", separator: " << request->separator();
    
    std::string result = request->first();
    if (!request->separator().empty()) {
        result += request->separator();
    }
    result += request->second();
    
    response->set_result(result);
    return grpc::Status::OK;
}

bool EchoServer::RegisterWithDiscovery(const std::string& discovery_endpoint, 
                                      int port, 
                                      const std::string& ifex_schema) {
    try {
        LOG(INFO) << "Registering Echo Service with discovery on port " << port;
        
        // Get primary IP address instead of using localhost
        std::string primary_ip = ifex::network::get_primary_ip_address();
        if (primary_ip.empty()) {
            LOG(WARNING) << "Could not determine primary IP address, falling back to localhost";
            primary_ip = "localhost";
        }
        
        // Create channel to discovery service
        auto channel = grpc::CreateChannel(discovery_endpoint, grpc::InsecureChannelCredentials());
        auto stub = swdv::service_discovery::register_service_service::NewStub(channel);
        
        // Create service info
        swdv::service_discovery::register_service_request request;
        auto* service_info = request.mutable_service_info();
        
        service_info->set_name("echo_service");
        service_info->set_version("1.0.0");
        service_info->set_description("Simple echo service for testing IFEX dynamic invocation");
        service_info->set_status(swdv::service_discovery::service_status_t::AVAILABLE);
        service_info->set_ifex_schema(ifex_schema);
        
        // Set endpoint with actual IP address
        auto* endpoint = service_info->mutable_endpoint();
        endpoint->set_address(primary_ip + ":" + std::to_string(port));
        endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
        
        LOG(INFO) << "Registering with endpoint: " << endpoint->address();
        
        // Make the call
        swdv::service_discovery::register_service_response response;
        grpc::ClientContext context;
        
        auto status = stub->register_service(&context, request, &response);
        
        if (status.ok() && !response.registration_id().empty()) {
            registration_id_ = response.registration_id();
            LOG(INFO) << "Successfully registered with service discovery, ID: " << registration_id_;
            return true;
        } else {
            LOG(ERROR) << "Failed to register with service discovery: " << status.error_message();
            return false;
        }
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Registration failed: " << e.what();
        return false;
    }
}

} // namespace ifex::test