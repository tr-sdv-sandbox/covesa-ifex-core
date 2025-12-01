#include "scheduler_server.hpp"
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <glog/logging.h>
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <csignal>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>

std::unique_ptr<grpc::Server> server;
std::atomic<bool> shutdown_requested{false};
std::mutex shutdown_mutex;
std::condition_variable shutdown_cv;

void SignalHandler(int signal) {
    LOG(INFO) << "\nShutting down IFEX scheduler service...";
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
        // Try multiple paths relative to common execution locations
        std::vector<std::string> paths = {
            "reference-services/ifex/ifex-scheduler-service.yml",  // From project root
            "../reference-services/ifex/ifex-scheduler-service.yml",  // From build dir
            "../../reference-services/ifex/ifex-scheduler-service.yml",  // From build subdirectory
            "../ifex/ifex-scheduler-service.yml",           // Legacy path
            "ifex/ifex-scheduler-service.yml",              // Legacy path
            "./ifex-scheduler-service.yml"                  // Current directory
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

void RunServer(int port, const std::string& service_discovery_endpoint) {
    LOG(INFO) << "IFEX Scheduler Service (C++)";
    LOG(INFO) << "  - Port: " << (port == 0 ? "auto-assigned" : std::to_string(port));
    LOG(INFO) << "  - Service discovery: " << service_discovery_endpoint;
    LOG(INFO) << "  - Calendar-style scheduler for IFEX services";
    LOG(INFO) << "  - CRUD operations, cron expressions, calendar views";
    LOG(INFO) << "  - Press Ctrl+C to stop";
    LOG(INFO) << "";

    // Load IFEX definition
    std::string ifex_schema = LoadIFEXDefinition();

    // Create service with discovery endpoint
    auto service = std::make_unique<ifex::reference::SchedulerServer>(service_discovery_endpoint);

    // Enable gRPC reflection
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    // Build server
    grpc::ServerBuilder builder;

    // Listen on the given address without authentication
    int actual_port;
    builder.AddListeningPort("0.0.0.0:" + std::to_string(port),
                            grpc::InsecureServerCredentials(),
                            &actual_port);

    // Register services (IFEX generates separate services per method)
    builder.RegisterService(static_cast<swdv::ifex_scheduler::create_job_service::Service*>(service.get()));
    builder.RegisterService(static_cast<swdv::ifex_scheduler::get_jobs_service::Service*>(service.get()));
    builder.RegisterService(static_cast<swdv::ifex_scheduler::get_job_service::Service*>(service.get()));
    builder.RegisterService(static_cast<swdv::ifex_scheduler::update_job_service::Service*>(service.get()));
    builder.RegisterService(static_cast<swdv::ifex_scheduler::delete_job_service::Service*>(service.get()));
    builder.RegisterService(static_cast<swdv::ifex_scheduler::get_calendar_view_service::Service*>(service.get()));

    // Build and start the server
    server = builder.BuildAndStart();

    if (!server) {
        LOG(ERROR) << "Failed to start server";
        return;
    }

    LOG(INFO) << "IFEX Scheduler Service listening on port " << actual_port;

    // Start job executor
    service->StartExecutor();

    // Register with service discovery
    if (service->RegisterWithDiscovery(actual_port, ifex_schema)) {
        LOG(INFO) << "";
        LOG(INFO) << "IFEX Scheduler Service is running and registered";
        LOG(INFO) << "  CRUD API: create_job, get_jobs, get_job, update_job, delete_job";
        LOG(INFO) << "  Calendar views: get_calendar_view (day/week/month)";
        LOG(INFO) << "  Cron expressions and recurring jobs supported";

        // Wait for server shutdown with periodic checks
        while (!shutdown_requested) {
            std::unique_lock<std::mutex> lock(shutdown_mutex);
            shutdown_cv.wait_for(lock, std::chrono::seconds(1));
        }

        // Graceful shutdown
        LOG(INFO) << "Initiating graceful shutdown...";
        server->Shutdown();

        // Give ongoing RPCs time to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } else {
        LOG(ERROR) << "Failed to register with service discovery";
        server->Shutdown();
    }

    // Cleanup
    service->StopExecutor();
    LOG(INFO) << "IFEX Scheduler Service stopped";
}

int main(int argc, char** argv) {
    // Initialize Google's logging library
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    FLAGS_colorlogtostderr = true;

    // Set up signal handler
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // Configuration
    int port = 0;  // Let OS assign port by default
    std::string service_discovery_endpoint;

    // Parse command line arguments (supports --key=value format)
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--port=", 0) == 0) {
            port = std::stoi(arg.substr(7));
        } else if (arg.rfind("--discovery=", 0) == 0) {
            service_discovery_endpoint = arg.substr(12);
        } else if (arg == "--help") {
            std::cout << "IFEX Scheduler Service\n"
                      << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --port=<port>        Server port (default: auto-assigned)\n"
                      << "  --discovery=<addr>   Service discovery endpoint (overrides SERVICE_DISCOVERY_ENDPOINT env var)\n"
                      << "  --help               Show this help message\n"
                      << "\n"
                      << "Environment variables:\n"
                      << "  SERVICE_DISCOVERY_ENDPOINT   Service discovery address (required if --discovery not provided)\n";
            return 0;
        }
    }

    // Get service discovery endpoint - command line takes precedence over environment
    if (service_discovery_endpoint.empty()) {
        const char* sd_env = std::getenv("SERVICE_DISCOVERY_ENDPOINT");
        if (sd_env) {
            service_discovery_endpoint = sd_env;
        }
    }

    // Validate that we have a service discovery endpoint
    if (service_discovery_endpoint.empty()) {
        LOG(ERROR) << "Service discovery endpoint must be provided via --discovery option or SERVICE_DISCOVERY_ENDPOINT environment variable";
        std::cerr << "Error: Service discovery endpoint must be provided via --discovery option or SERVICE_DISCOVERY_ENDPOINT environment variable\n";
        std::cerr << "Run with --help for usage information\n";
        return 1;
    }

    LOG(INFO) << "Using service discovery endpoint: " << service_discovery_endpoint;

    try {
        RunServer(port, service_discovery_endpoint);
    } catch (const std::exception& e) {
        LOG(ERROR) << "Server failed: " << e.what();
        return 1;
    }

    return 0;
}
