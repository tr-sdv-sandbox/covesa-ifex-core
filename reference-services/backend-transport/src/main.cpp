#include "backend_transport_server.hpp"

#include <glog/logging.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

std::unique_ptr<grpc::Server> server;
std::unique_ptr<ifex::reference::BackendTransportServer> backend_service;
std::atomic<bool> shutdown_requested{false};
std::mutex shutdown_mutex;
std::condition_variable shutdown_cv;

void SignalHandler(int signal) {
    LOG(INFO) << "\nShutting down IFEX Backend Transport Service...";
    shutdown_requested = true;
    shutdown_cv.notify_all();

    // Give the main thread a moment to initiate shutdown
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (server) {
        server->Shutdown();
    }
}

std::string LoadIFEXDefinition() {
    try {
        std::vector<std::string> paths = {
            "reference-services/ifex/backend-transport-service.yml",
            "../reference-services/ifex/backend-transport-service.yml",
            "../../reference-services/ifex/backend-transport-service.yml",
            "../ifex/backend-transport-service.yml",
            "ifex/backend-transport-service.yml",
            "./backend-transport-service.yml"
        };

        for (const auto& path : paths) {
            std::ifstream file(path);
            if (file.is_open()) {
                LOG(INFO) << "Found IFEX definition at: " << path;
                std::stringstream buffer;
                buffer << file.rdbuf();
                return buffer.str();
            }
        }

        LOG(WARNING) << "Could not load IFEX definition from any of the standard paths";
        return "";
    } catch (const std::exception& e) {
        LOG(WARNING) << "Failed to load IFEX definition: " << e.what();
        return "";
    }
}

std::string GetEnvOrDefault(const char* name, const std::string& default_value) {
    const char* value = std::getenv(name);
    return value ? value : default_value;
}

int GetEnvIntOrDefault(const char* name, int default_value) {
    const char* value = std::getenv(name);
    if (value) {
        try {
            return std::stoi(value);
        } catch (...) {
            LOG(WARNING) << "Invalid integer value for " << name << ": " << value;
        }
    }
    return default_value;
}

void PrintUsage(const char* program) {
    std::cout << "IFEX Backend Transport Service\n"
              << "Usage: " << program << " [options]\n"
              << "Options:\n"
              << "  --port=<port>            gRPC server port (default: auto-assigned)\n"
              << "  --mqtt-host=<host>       MQTT broker host (default: localhost)\n"
              << "  --mqtt-port=<port>       MQTT broker port (default: 1883)\n"
              << "  --mqtt-user=<user>       MQTT username\n"
              << "  --mqtt-pass=<pass>       MQTT password\n"
              << "  --vehicle-id=<id>        Vehicle identifier (default: vehicle-001)\n"
              << "  --queue-size=<size>      Queue size per content_id (default: 1000)\n"
              << "  --persistence-dir=<dir>  Directory for message persistence\n"
              << "  --discovery=<addr>       Service discovery endpoint\n"
              << "  --help                   Show this help message\n"
              << "\n"
              << "Environment variables (override defaults, command line overrides env):\n"
              << "  MQTT_HOST                MQTT broker hostname\n"
              << "  MQTT_PORT                MQTT broker port\n"
              << "  MQTT_USERNAME            MQTT username\n"
              << "  MQTT_PASSWORD            MQTT password\n"
              << "  VEHICLE_ID               Vehicle identifier for MQTT topics\n"
              << "  QUEUE_SIZE               Queue size per content_id\n"
              << "  PERSISTENCE_DIR          Directory for message persistence\n"
              << "  SERVICE_DISCOVERY_ENDPOINT  Service discovery address\n";
}

void RunServer(ifex::reference::BackendTransportServer::Config& config, int port) {
    LOG(INFO) << "IFEX Backend Transport Service (C++)";
    LOG(INFO) << "  - gRPC Port: " << (port == 0 ? "auto-assigned" : std::to_string(port));
    LOG(INFO) << "  - MQTT Broker: " << config.mqtt_host << ":" << config.mqtt_port;
    LOG(INFO) << "  - Vehicle ID: " << config.vehicle_id;
    LOG(INFO) << "  - Queue Size: " << config.queue_size_per_content_id << " per content_id";
    LOG(INFO) << "  - Persistence Dir: " << config.persistence_dir;
    LOG(INFO) << "  - Press Ctrl+C to stop";
    LOG(INFO) << "";

    // Load IFEX definition
    std::string ifex_schema = LoadIFEXDefinition();

    // Create service
    backend_service = std::make_unique<ifex::reference::BackendTransportServer>(config);

    // Start the backend service (MQTT connection, queue manager)
    if (!backend_service->Start()) {
        LOG(ERROR) << "Failed to start backend transport service";
        return;
    }

    // Enable gRPC reflection
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    // Build gRPC server
    grpc::ServerBuilder builder;

    int actual_port;
    builder.AddListeningPort("0.0.0.0:" + std::to_string(port),
                             grpc::InsecureServerCredentials(),
                             &actual_port);

    // Register all IFEX method services
    builder.RegisterService(static_cast<swdv::backend_transport_service::publish_service::Service*>(backend_service.get()));
    builder.RegisterService(static_cast<swdv::backend_transport_service::get_connection_status_service::Service*>(backend_service.get()));
    builder.RegisterService(static_cast<swdv::backend_transport_service::get_queue_status_service::Service*>(backend_service.get()));
    builder.RegisterService(static_cast<swdv::backend_transport_service::get_stats_service::Service*>(backend_service.get()));
    builder.RegisterService(static_cast<swdv::backend_transport_service::healthy_service::Service*>(backend_service.get()));

    // Register streaming event services
    builder.RegisterService(static_cast<swdv::backend_transport_service::on_content_service::Service*>(backend_service.get()));
    builder.RegisterService(static_cast<swdv::backend_transport_service::on_ack_service::Service*>(backend_service.get()));
    builder.RegisterService(static_cast<swdv::backend_transport_service::on_connection_changed_service::Service*>(backend_service.get()));
    builder.RegisterService(static_cast<swdv::backend_transport_service::on_queue_status_changed_service::Service*>(backend_service.get()));

    // Build and start gRPC server
    server = builder.BuildAndStart();

    if (!server) {
        LOG(ERROR) << "Failed to start gRPC server";
        backend_service->Stop();
        return;
    }

    LOG(INFO) << "IFEX Backend Transport Service listening on port " << actual_port;

    // Register with service discovery (optional)
    if (!config.discovery_endpoint.empty()) {
        if (backend_service->RegisterWithDiscovery(actual_port, ifex_schema)) {
            LOG(INFO) << "Registered with service discovery at " << config.discovery_endpoint;
        } else {
            LOG(WARNING) << "Failed to register with service discovery";
        }
    }

    LOG(INFO) << "";
    LOG(INFO) << "IFEX Backend Transport Service is running";
    LOG(INFO) << "  Methods: publish, get_connection_status, get_queue_status, get_stats, healthy";
    LOG(INFO) << "  Events:  on_content, on_ack, on_connection_changed, on_queue_status_changed";
    LOG(INFO) << "";

    // Wait for shutdown
    while (!shutdown_requested) {
        std::unique_lock<std::mutex> lock(shutdown_mutex);
        shutdown_cv.wait_for(lock, std::chrono::seconds(1));
    }

    // Graceful shutdown
    LOG(INFO) << "Initiating graceful shutdown...";

    // Stop gRPC server first (stops accepting new requests)
    server->Shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Stop backend service (persists pending messages)
    backend_service->Stop();

    LOG(INFO) << "IFEX Backend Transport Service stopped";
}

int main(int argc, char** argv) {
    // Initialize Google's logging library
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    FLAGS_colorlogtostderr = true;

    // Set up signal handlers
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // Configuration with environment variable defaults
    ifex::reference::BackendTransportServer::Config config;
    config.mqtt_host = GetEnvOrDefault("MQTT_HOST", "localhost");
    config.mqtt_port = GetEnvIntOrDefault("MQTT_PORT", 1883);
    config.mqtt_username = GetEnvOrDefault("MQTT_USERNAME", "");
    config.mqtt_password = GetEnvOrDefault("MQTT_PASSWORD", "");
    config.vehicle_id = GetEnvOrDefault("VEHICLE_ID", "vehicle-001");
    config.queue_size_per_content_id = static_cast<size_t>(GetEnvIntOrDefault("QUEUE_SIZE", 1000));
    config.persistence_dir = GetEnvOrDefault("PERSISTENCE_DIR", "/var/lib/ifex/backend-transport");
    config.discovery_endpoint = GetEnvOrDefault("SERVICE_DISCOVERY_ENDPOINT", "");

    int port = 0;  // Auto-assign by default

    // Parse command line arguments (override env vars)
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg.rfind("--port=", 0) == 0) {
            port = std::stoi(arg.substr(7));
        } else if (arg.rfind("--mqtt-host=", 0) == 0) {
            config.mqtt_host = arg.substr(12);
        } else if (arg.rfind("--mqtt-port=", 0) == 0) {
            config.mqtt_port = std::stoi(arg.substr(12));
        } else if (arg.rfind("--mqtt-user=", 0) == 0) {
            config.mqtt_username = arg.substr(12);
        } else if (arg.rfind("--mqtt-pass=", 0) == 0) {
            config.mqtt_password = arg.substr(12);
        } else if (arg.rfind("--vehicle-id=", 0) == 0) {
            config.vehicle_id = arg.substr(13);
        } else if (arg.rfind("--queue-size=", 0) == 0) {
            config.queue_size_per_content_id = std::stoul(arg.substr(13));
        } else if (arg.rfind("--persistence-dir=", 0) == 0) {
            config.persistence_dir = arg.substr(18);
        } else if (arg.rfind("--discovery=", 0) == 0) {
            config.discovery_endpoint = arg.substr(12);
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            std::cerr << "Run with --help for usage information\n";
            return 1;
        }
    }

    try {
        RunServer(config, port);
    } catch (const std::exception& e) {
        LOG(ERROR) << "Server failed: " << e.what();
        return 1;
    }

    return 0;
}
