#include "echo_server.hpp"
#include <glog/logging.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <csignal>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <chrono>

std::unique_ptr<ifex::test::EchoServer> g_server;
std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int signal) {
    LOG(INFO) << "Received signal " << signal << ", shutting down...";
    g_shutdown_requested.store(true);
}

int main(int argc, char* argv[]) {
    // Initialize Google logging
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();
    
    // Set up signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Parse command line arguments
    std::string listen_address = "0.0.0.0:50053";
    std::string discovery_endpoint = "localhost:50051";
    std::string ifex_schema_path;
    bool register_with_discovery = true;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.find("--listen=") == 0) {
            listen_address = arg.substr(9);
        } else if (arg.find("--discovery=") == 0) {
            discovery_endpoint = arg.substr(12);
        } else if (arg.find("--ifex-schema=") == 0) {
            ifex_schema_path = arg.substr(14);
        } else if (arg == "--no-discovery") {
            register_with_discovery = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --listen=ADDRESS      Listen address (default: 0.0.0.0:50053)\n"
                      << "  --discovery=ENDPOINT  Discovery service endpoint (default: localhost:50051)\n"
                      << "  --ifex-schema=PATH    Path to IFEX schema file\n"
                      << "  --no-discovery        Don't register with discovery service\n"
                      << "  --help, -h           Show this help message\n"
                      << "Environment:\n"
                      << "  IFEX_SCHEMA_DIR       Directory containing IFEX schema files\n";
            return 0;
        }
    }
    
    LOG(INFO) << "Starting Echo Test Service on " << listen_address;
    
    try {
        // Create and start the server
        g_server = std::make_unique<ifex::test::EchoServer>();
        g_server->Start(listen_address);
        
        // Register with discovery if requested
        if (register_with_discovery) {
            LOG(INFO) << "Registering with discovery service at " << discovery_endpoint;
            
            // Read IFEX schema from file
            std::string ifex_schema;
            bool schema_found = false;
            
            // First try command-line specified path
            if (!ifex_schema_path.empty()) {
                std::ifstream schema_file(ifex_schema_path);
                if (schema_file.is_open()) {
                    std::stringstream buffer;
                    buffer << schema_file.rdbuf();
                    ifex_schema = buffer.str();
                    schema_file.close();
                    LOG(INFO) << "Successfully loaded IFEX schema from command-line path: " << ifex_schema_path;
                    schema_found = true;
                } else {
                    LOG(ERROR) << "Failed to open IFEX schema file specified on command line: " << ifex_schema_path;
                }
            }
            
            // If not found, try environment variable
            if (!schema_found) {
                const char* schema_dir_env = std::getenv("IFEX_SCHEMA_DIR");
                if (schema_dir_env) {
                    std::string schema_path = std::string(schema_dir_env) + "/echo_service.ifex.yml";
                    std::ifstream schema_file(schema_path);
                    if (schema_file.is_open()) {
                        std::stringstream buffer;
                        buffer << schema_file.rdbuf();
                        ifex_schema = buffer.str();
                        schema_file.close();
                        LOG(INFO) << "Successfully loaded IFEX schema from IFEX_SCHEMA_DIR: " << schema_path;
                        schema_found = true;
                    } else {
                        LOG(WARNING) << "IFEX_SCHEMA_DIR set but schema not found at: " << schema_path;
                    }
                }
            }
            
            if (!schema_found) {
                LOG(ERROR) << "Failed to find IFEX schema file";
                LOG(ERROR) << "Please specify schema path using --ifex-schema=PATH or set IFEX_SCHEMA_DIR environment variable";
                LOG(ERROR) << "Current working directory: " << std::filesystem::current_path();
                return 1;
            }
            
            // Extract port from listen address
            size_t colon_pos = listen_address.find_last_of(':');
            int port = 50053;
            if (colon_pos != std::string::npos) {
                port = std::stoi(listen_address.substr(colon_pos + 1));
            }
            
            if (!g_server->RegisterWithDiscovery(discovery_endpoint, port, ifex_schema)) {
                LOG(ERROR) << "Failed to register echo service with discovery service";
                LOG(ERROR) << "Discovery endpoint: " << discovery_endpoint;
                LOG(ERROR) << "Service port: " << port;
            } else {
                LOG(INFO) << "Successfully registered echo service with discovery service";
            }
        }
        
        LOG(INFO) << "Echo service is running. Press Ctrl+C to stop.";
        
        // Wait for shutdown signal
        while (!g_shutdown_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        LOG(INFO) << "Shutdown requested, stopping server...";
        g_server->Shutdown();
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to start echo service: " << e.what();
        return 1;
    }
    
    LOG(INFO) << "Echo service stopped.";
    return 0;
}