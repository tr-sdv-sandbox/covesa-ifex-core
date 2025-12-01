#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>

#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include "climate_comfort_server.hpp"

// Configuration
static std::string address = "0.0.0.0:50062";
static std::string discovery_address = "localhost:50051";
static std::string ifex_schema = "climate-comfort-service.ifex.yml";

std::unique_ptr<grpc::Server> server;
std::unique_ptr<swdv::climate_comfort_service::ClimateComfortServiceImpl> service_impl;

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
        service_impl = std::make_unique<swdv::climate_comfort_service::ClimateComfortServiceImpl>();
        
        // Start service background tasks
        service_impl->Start();
        
        // Build and start gRPC server
        grpc::ServerBuilder builder;
        builder.AddListeningPort(address, grpc::InsecureServerCredentials());
        
        // Register all service interfaces (IFEX generates one per method)
        builder.RegisterService(static_cast<swdv::climate_comfort_service::set_comfort_service::Service*>(service_impl.get()));
        builder.RegisterService(static_cast<swdv::climate_comfort_service::quick_comfort_adjustment_service::Service*>(service_impl.get()));
        builder.RegisterService(static_cast<swdv::climate_comfort_service::optimize_for_efficiency_service::Service*>(service_impl.get()));
        builder.RegisterService(static_cast<swdv::climate_comfort_service::get_comfort_status_service::Service*>(service_impl.get()));
        builder.RegisterService(static_cast<swdv::climate_comfort_service::prepare_for_occupancy_service::Service*>(service_impl.get()));
        
        server = builder.BuildAndStart();
        if (!server) {
            LOG(ERROR) << "Failed to start server on " << address;
            return 1;
        }
        
        LOG(INFO) << "Climate comfort service listening on " << address;
        
        // Load IFEX schema if provided
        std::string schema_path = ifex_schema;
        if (schema_path.empty()) {
            schema_path = "./climate-comfort-service.ifex.yml";
        }
        LOG(INFO) << "Successfully loaded IFEX schema from: " << schema_path;
        
        // Get actual bound port
        int bound_port = 50062;
        size_t colon_pos = address.find_last_of(':');
        if (colon_pos != std::string::npos) {
            std::string port_str = address.substr(colon_pos + 1);
            std::istringstream iss(port_str);
            iss >> bound_port;
        }
        
        // Register with discovery service
        LOG(INFO) << "Registering Climate Comfort Service with discovery on port " << bound_port;
        if (service_impl->RegisterWithDiscovery(discovery_address, bound_port, schema_path)) {
            LOG(INFO) << "Successfully registered with discovery service";
        } else {
            LOG(WARNING) << "Failed to register with discovery service, continuing anyway";
        }
        
        // Wait for shutdown signal
        {
            std::unique_lock<std::mutex> lock(shutdown_mutex);
            shutdown_cv.wait(lock, []{ return shutdown_requested; });
        }
        
        // Graceful shutdown
        LOG(INFO) << "Initiating graceful shutdown...";
        
        // First stop the service to prevent new requests
        if (service_impl) {
            service_impl->Stop();
        }
        
        // Then shutdown the server
        if (server) {
            // Give server time to finish ongoing RPCs
            auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
            server->Shutdown(deadline);
            // Wait for server to fully stop
            server->Wait();
        }
        
        // Reset the server and service pointers to ensure proper cleanup
        server.reset();
        service_impl.reset();
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception: " << e.what();
        return 1;
    }
    
    LOG(INFO) << "Climate comfort service shutdown complete";
    
    // Give logging a moment to flush
    google::FlushLogFiles(google::INFO);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    return 0;
}