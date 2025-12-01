#include <grpcpp/grpcpp.h>
#include <glog/logging.h>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <fstream>

#include "ifex/discovery.hpp"
#include "ifex/network.hpp"
#include "settings_server.hpp"
#include "service-discovery-service.grpc.pb.h"

using json = nlohmann::json;

// Global flag for graceful shutdown
std::atomic<bool> shutdown_requested(false);

void signal_handler(int signal) {
    LOG(INFO) << "Received signal " << signal << ", shutting down...";
    shutdown_requested = true;
}

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    
    // Parse command line arguments
    std::string listen_address = "0.0.0.0:50055";
    std::string discovery_endpoint = "localhost:50051";
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        LOG(INFO) << "Arg[" << i << "]: " << arg;
        if (arg.substr(0, 9) == "--listen=") {
            listen_address = arg.substr(9);
        } else if (arg.substr(0, 12) == "--discovery=") {
            discovery_endpoint = arg.substr(12);
        }
    }
    
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    try {
        LOG(INFO) << "Starting Settings Service";
        LOG(INFO) << "Listen address: " << listen_address;
        LOG(INFO) << "Discovery endpoint: " << discovery_endpoint;
        
        // Create and initialize settings server
        auto settings_server = std::make_unique<ifex::settings::SettingsServer>();
        
        // Load initial presets
        settings_server->LoadPresetsFromFile("climate_control_presets.json");
        
        // Parse listen address
        auto colon_pos = listen_address.find(':');
        if (colon_pos == std::string::npos) {
            LOG(ERROR) << "Invalid listen address format: " << listen_address;
            return 1;
        }
        
        std::string host = listen_address.substr(0, colon_pos);
        int port = std::stoi(listen_address.substr(colon_pos + 1));
        
        // Get actual IP address for registration
        std::string primary_ip = ifex::network::get_primary_ip_address();
        if (primary_ip.empty()) {
            LOG(WARNING) << "Could not determine primary IP address, using localhost";
            primary_ip = "localhost";
        }
        
        if (host == "0.0.0.0") {
            host = primary_ip;
        }
        
        std::string service_address = host + ":" + std::to_string(port);
        
        // Start the gRPC server
        LOG(INFO) << "Starting gRPC server on " << listen_address;
        settings_server->Start(listen_address);
        LOG(INFO) << "gRPC server started successfully";
        
        // Read IFEX schema
        std::ifstream schema_file("settings-service.ifex.yml");
        if (!schema_file.is_open()) {
            LOG(ERROR) << "Failed to open settings-service.ifex.yml";
            return 1;
        }
        std::string ifex_schema((std::istreambuf_iterator<char>(schema_file)),
                               std::istreambuf_iterator<char>());
        schema_file.close();
        
        // Register with discovery service using gRPC directly (like echo service)
        try {
            auto channel = grpc::CreateChannel(discovery_endpoint, grpc::InsecureChannelCredentials());
            auto stub = swdv::service_discovery::register_service_service::NewStub(channel);
            
            swdv::service_discovery::register_service_request request;
            auto* service_info = request.mutable_service_info();
            
            service_info->set_name("settings_service");
            service_info->set_version("1.0");
            service_info->set_description("Generic settings and preset management service");
            service_info->set_status(swdv::service_discovery::service_status_t::AVAILABLE);
            service_info->set_ifex_schema(ifex_schema);
            
            auto* endpoint = service_info->mutable_endpoint();
            endpoint->set_address(service_address);
            endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
            
            LOG(INFO) << "Registering with endpoint: " << endpoint->address();
            
            swdv::service_discovery::register_service_response response;
            grpc::ClientContext context;
            
            auto status = stub->register_service(&context, request, &response);
            
            if (status.ok() && !response.registration_id().empty()) {
                LOG(INFO) << "Successfully registered with service discovery, ID: " << response.registration_id();
            } else {
                LOG(ERROR) << "Failed to register with service discovery: " << status.error_message();
            }
            
        } catch (const std::exception& e) {
            LOG(ERROR) << "Registration failed: " << e.what();
        }
        
        LOG(INFO) << "Settings Service ready at " << service_address;
        
        // Keep running
        while (!shutdown_requested) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        // Cleanup
        LOG(INFO) << "Shutting down settings service...";
        settings_server->Shutdown();
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception: " << e.what();
        return 1;
    }
    
    LOG(INFO) << "Settings service stopped";
    return 0;
}