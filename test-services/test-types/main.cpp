#include "test_types_server.hpp"
#include <ifex/discovery.hpp>
#include <ifex/network.hpp>
#include <glog/logging.h>
#include <csignal>
#include <thread>
#include <chrono>
#include <fstream>

std::unique_ptr<test_types::TestTypesServer> g_server;
std::atomic<bool> g_shutdown(false);

void signal_handler(int signal) {
    LOG(INFO) << "Received signal " << signal << ", shutting down...";
    g_shutdown = true;
}

int main(int argc, char** argv) {
    // Early debug output to stderr
    fprintf(stderr, "test-types service starting with %d args\n", argc);
    for (int i = 0; i < argc; i++) {
        fprintf(stderr, "  arg[%d]: %s\n", i, argv[i]);
    }
    
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 0;
    FLAGS_log_dir = "/tmp";
    
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Parse command line arguments
    std::string listen_address = "0.0.0.0:50060";
    std::string discovery_endpoint;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        LOG(INFO) << "Arg[" << i << "]: " << arg;
        
        if (arg.substr(0, 9) == "--listen=") {
            listen_address = arg.substr(9);
        } else if (arg.substr(0, 12) == "--discovery=") {
            discovery_endpoint = arg.substr(12);
        }
    }
    
    fprintf(stderr, "After arg parsing, listen=%s, discovery=%s\n", 
            listen_address.c_str(), discovery_endpoint.c_str());
    
    LOG(INFO) << "Starting Test Types Service";
    LOG(INFO) << "Listen address: " << listen_address;
    LOG(INFO) << "Discovery endpoint: " << discovery_endpoint;
    
    // Extract port from listen address
    size_t colon_pos = listen_address.find_last_of(':');
    if (colon_pos == std::string::npos) {
        LOG(ERROR) << "Invalid listen address format";
        fprintf(stderr, "Invalid listen address format\n");
        return 1;
    }
    int port = std::stoi(listen_address.substr(colon_pos + 1));
    
    fprintf(stderr, "About to create TestTypesServer\n");
    
    // Start the server
    g_server = std::make_unique<test_types::TestTypesServer>();
    
    fprintf(stderr, "About to start server on %s\n", listen_address.c_str());
    g_server->Start(listen_address);
    
    fprintf(stderr, "Server started successfully\n");
    
    LOG(INFO) << "gRPC server started successfully";
    
    // Register with discovery service if endpoint provided
    std::unique_ptr<ifex::DiscoveryClient> discovery_client;
    if (!discovery_endpoint.empty()) {
        try {
            discovery_client = ifex::DiscoveryClient::create(discovery_endpoint);
            
            // Load IFEX schema
            std::string ifex_schema;
            std::ifstream schema_file("test-types-service.ifex.yml");
            if (schema_file.is_open()) {
                std::stringstream buffer;
                buffer << schema_file.rdbuf();
                ifex_schema = buffer.str();
            } else {
                LOG(ERROR) << "Failed to load IFEX schema file";
                return 1;
            }
            
            // Get primary IP
            std::string primary_ip = ifex::network::get_primary_ip_address();
            std::string endpoint_address = primary_ip + ":" + std::to_string(port);
            
            LOG(INFO) << "Registering with endpoint: " << endpoint_address;
            
            // Register service
            ifex::ServiceEndpoint service_endpoint;
            service_endpoint.address = endpoint_address;
            service_endpoint.transport = ifex::ServiceEndpoint::Transport::GRPC;
            
            auto reg_id = discovery_client->register_service(service_endpoint, ifex_schema);
            
            if (!reg_id.empty()) {
                LOG(INFO) << "Successfully registered with service discovery, ID: " << reg_id;
            } else {
                LOG(ERROR) << "Failed to register with service discovery";
            }
            
        } catch (const std::exception& e) {
            LOG(ERROR) << "Discovery registration failed: " << e.what();
        }
    }
    
    LOG(INFO) << "Test Types Service ready at " << listen_address;
    
    // Keep running until shutdown
    while (!g_shutdown) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    LOG(INFO) << "Shutting down test types service...";
    g_server->Shutdown();
    g_server.reset();
    
    LOG(INFO) << "Test types service stopped";
    return 0;
}