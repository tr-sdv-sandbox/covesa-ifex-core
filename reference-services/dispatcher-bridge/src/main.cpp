/**
 * @file main.cpp
 * @brief Entry point for IFEX Dispatcher Bridge service
 *
 * The Dispatcher Bridge enables cloud applications to invoke any onboard IFEX
 * service by forwarding RPC requests received via Backend Transport to the
 * local Dispatcher service.
 */

#include "dispatcher_bridge.hpp"
#include "ifex_content_ids.hpp"

#include <glog/logging.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

std::unique_ptr<ifex::reference::DispatcherBridge> g_bridge;
std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int signal) {
    LOG(INFO) << "Received signal " << signal << ", shutting down...";
    g_shutdown_requested.store(true);
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "\n"
              << "IFEX Dispatcher Bridge - forwards cloud RPC requests to onboard services\n"
              << "\n"
              << "Options:\n"
              << "  --dispatcher=ENDPOINT     Dispatcher service endpoint (default: localhost:50052)\n"
              << "  --backend-transport=ENDPOINT\n"
              << "                            Backend Transport endpoint (default: localhost:50060)\n"
              << "  --content-id=ID           Content ID for RPC channel (default: "
              << ifex::content_id::DISPATCHER_RPC << ")\n"
              << "  --max-concurrent=N        Max concurrent requests (default: 100)\n"
              << "  --default-timeout=MS      Default request timeout (default: 30000)\n"
              << "  --workers=N               Number of worker threads (default: 4)\n"
              << "  --help, -h                Show this help message\n";
}

int main(int argc, char* argv[]) {
    // Initialize Google logging
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();

    // Set up signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Default configuration
    ifex::reference::DispatcherBridgeConfig config;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg.find("--dispatcher=") == 0) {
            config.dispatcher_endpoint = arg.substr(13);
        } else if (arg.find("--backend-transport=") == 0) {
            config.backend_transport_endpoint = arg.substr(20);
        } else if (arg.find("--content-id=") == 0) {
            config.rpc_content_id = static_cast<uint32_t>(std::stoul(arg.substr(13)));
        } else if (arg.find("--max-concurrent=") == 0) {
            config.max_concurrent_requests = static_cast<uint32_t>(std::stoul(arg.substr(17)));
        } else if (arg.find("--default-timeout=") == 0) {
            config.default_timeout_ms = static_cast<uint32_t>(std::stoul(arg.substr(18)));
        } else if (arg.find("--workers=") == 0) {
            config.num_workers = static_cast<uint32_t>(std::stoul(arg.substr(10)));
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            LOG(ERROR) << "Unknown argument: " << arg;
            print_usage(argv[0]);
            return 1;
        }
    }

    LOG(INFO) << "Starting IFEX Dispatcher Bridge";
    LOG(INFO) << "  Dispatcher endpoint: " << config.dispatcher_endpoint;
    LOG(INFO) << "  Backend Transport endpoint: " << config.backend_transport_endpoint;
    LOG(INFO) << "  RPC content_id: " << config.rpc_content_id;
    LOG(INFO) << "  Max concurrent requests: " << config.max_concurrent_requests;
    LOG(INFO) << "  Default timeout: " << config.default_timeout_ms << "ms";
    LOG(INFO) << "  Worker threads: " << config.num_workers;

    try {
        // Create and start the bridge
        g_bridge = std::make_unique<ifex::reference::DispatcherBridge>(config);

        if (!g_bridge->Start()) {
            LOG(ERROR) << "Failed to start Dispatcher Bridge";
            return 1;
        }

        LOG(INFO) << "Dispatcher Bridge is running. Press Ctrl+C to stop.";

        // Main loop - periodically log statistics
        auto last_stats_time = std::chrono::steady_clock::now();
        const auto stats_interval = std::chrono::seconds(60);

        while (!g_shutdown_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Log stats periodically
            auto now = std::chrono::steady_clock::now();
            if (now - last_stats_time >= stats_interval) {
                auto stats = g_bridge->GetStats();
                LOG(INFO) << "Stats: received=" << stats.requests_received
                          << " completed=" << stats.requests_completed
                          << " failed=" << stats.requests_failed
                          << " timedout=" << stats.requests_timed_out
                          << " rejected=" << stats.requests_rejected
                          << " pending=" << stats.pending_count;
                last_stats_time = now;
            }
        }

        LOG(INFO) << "Shutdown requested, stopping bridge...";
        g_bridge->Stop();

        // Final stats
        auto stats = g_bridge->GetStats();
        LOG(INFO) << "Final stats: received=" << stats.requests_received
                  << " completed=" << stats.requests_completed
                  << " failed=" << stats.requests_failed
                  << " timedout=" << stats.requests_timed_out
                  << " rejected=" << stats.requests_rejected;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Fatal error: " << e.what();
        return 1;
    }

    LOG(INFO) << "Dispatcher Bridge stopped.";
    return 0;
}
