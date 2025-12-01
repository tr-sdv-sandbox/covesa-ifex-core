#include "discovery_server.hpp"
#include <ifex/network.hpp>
#include <glog/logging.h>
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

std::unique_ptr<ifex::reference::DiscoveryServer> g_server;
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
    std::string listen_address = "0.0.0.0:50051";
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.find("--listen=") == 0) {
            listen_address = arg.substr(9);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --listen=ADDRESS      Listen address (default: 0.0.0.0:50051)\n"
                      << "  --help, -h           Show this help message\n";
            return 0;
        }
    }
    
    LOG(INFO) << "Starting IFEX Discovery Service on " << listen_address;
    
    try {
        // Create and start the server
        g_server = std::make_unique<ifex::reference::DiscoveryServer>();
        g_server->Start(listen_address);
        
        // If we bound to 0.0.0.0, show the actual IP addresses
        size_t colon_pos = listen_address.find_last_of(':');
        if (colon_pos != std::string::npos) {
            std::string host = listen_address.substr(0, colon_pos);
            std::string port = listen_address.substr(colon_pos + 1);
            
            if (host == "0.0.0.0" || host == "::" || host.empty()) {
                auto actual_endpoints = ifex::network::resolve_bind_address(listen_address);
                if (!actual_endpoints.empty()) {
                    LOG(INFO) << "Service accessible at:";
                    for (const auto& endpoint : actual_endpoints) {
                        LOG(INFO) << "  - " << endpoint;
                    }
                }
            }
        }
        
        LOG(INFO) << "Discovery service is running. Press Ctrl+C to stop.";
        
        // Wait for shutdown signal
        while (!g_shutdown_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        LOG(INFO) << "Shutdown requested, stopping server...";
        g_server->Shutdown();
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to start discovery service: " << e.what();
        return 1;
    }
    
    LOG(INFO) << "Discovery service stopped.";
    return 0;
}