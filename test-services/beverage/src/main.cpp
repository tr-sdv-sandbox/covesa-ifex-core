#include <csignal>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <condition_variable>
#include <mutex>

#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include "beverage_server.hpp"

// Configuration
static std::string address = "0.0.0.0:50061";
static std::string discovery_address = "localhost:50051";
static std::string ifex_schema = "beverage-service.ifex.yml";

std::unique_ptr<grpc::Server> server;
std::unique_ptr<swdv::beverage_service::BeverageServiceImpl> service_impl;

// Shutdown synchronization
std::mutex shutdown_mutex;
std::condition_variable shutdown_cv;
bool shutdown_requested = false;

void SignalHandler(int signal) {
    LOG(INFO) << "Received signal " << signal << ", shutting down...";
    {
        std::lock_guard<std::mutex> lock(shutdown_mutex);
        shutdown_requested = true;
    }
    shutdown_cv.notify_all();
}

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.find("--address=") == 0) {
            address = arg.substr(10);
        } else if (arg.find("--discovery_address=") == 0) {
            discovery_address = arg.substr(20);
        } else if (arg.find("--ifex_schema=") == 0) {
            ifex_schema = arg.substr(14);
        }
    }
    
    // Install signal handlers
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
    
    try {
        // Create service implementation
        service_impl = std::make_unique<swdv::beverage_service::BeverageServiceImpl>();
        
        // Start service background tasks
        service_impl->Start();
        
        // Build and start gRPC server
        grpc::ServerBuilder builder;
        builder.AddListeningPort(address, grpc::InsecureServerCredentials());
        
        // Register all service interfaces (IFEX generates one per method)
        builder.RegisterService(static_cast<swdv::beverage_service::prepare_beverage_service::Service*>(service_impl.get()));
        builder.RegisterService(static_cast<swdv::beverage_service::get_preparation_status_service::Service*>(service_impl.get()));
        builder.RegisterService(static_cast<swdv::beverage_service::cancel_preparation_service::Service*>(service_impl.get()));
        builder.RegisterService(static_cast<swdv::beverage_service::get_capabilities_service::Service*>(service_impl.get()));
        
        server = builder.BuildAndStart();
        if (!server) {
            LOG(ERROR) << "Failed to start server on " << address;
            return 1;
        }
        
        LOG(INFO) << "Beverage service listening on " << address;
        
        // Load IFEX schema for discovery registration
        std::string ifex_schema_content;
        std::ifstream schema_file(ifex_schema);
        if (schema_file.is_open()) {
            std::stringstream buffer;
            buffer << schema_file.rdbuf();
            ifex_schema_content = buffer.str();
            schema_file.close();
            LOG(INFO) << "Successfully loaded IFEX schema from: " << ifex_schema;
        } else {
            LOG(ERROR) << "Failed to open IFEX schema file: " << ifex_schema;
            // Continue without schema - service will still work
        }
        
        // Extract port from address
        size_t colon_pos = address.find_last_of(':');
        int port = 50061;
        if (colon_pos != std::string::npos) {
            port = std::stoi(address.substr(colon_pos + 1));
        }
        
        // Register with discovery service if available
        if (!discovery_address.empty()) {
            if (!service_impl->RegisterWithDiscovery(discovery_address, port, ifex_schema_content)) {
                LOG(ERROR) << "Failed to register with discovery service";
                LOG(ERROR) << "Discovery endpoint: " << discovery_address;
                LOG(ERROR) << "Service port: " << port;
                // Continue anyway - service can still work without discovery
            } else {
                LOG(INFO) << "Successfully registered with discovery service";
            }
        }
        
        // Wait for shutdown signal
        {
            std::unique_lock<std::mutex> lock(shutdown_mutex);
            shutdown_cv.wait(lock, []{ return shutdown_requested; });
        }
        
        // Graceful shutdown
        LOG(INFO) << "Initiating graceful shutdown...";
        server->Shutdown();
        
        // Cleanup
        service_impl->Stop();
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception: " << e.what();
        return 1;
    }
    
    LOG(INFO) << "Beverage service shutdown complete";
    return 0;
}