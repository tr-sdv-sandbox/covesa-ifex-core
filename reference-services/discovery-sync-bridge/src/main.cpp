/**
 * @file main.cpp
 * @brief Entry point for IFEX Discovery Sync Bridge service
 *
 * The Discovery Sync Bridge monitors the Discovery service and publishes
 * state changes to the cloud via Backend Transport. It supports:
 * - Initialization delay to wait for services to register
 * - Delta sync to minimize traffic
 * - Heartbeat for connection monitoring
 */

#include "discovery_sync_bridge.hpp"
#include "ifex_content_ids.hpp"

#include <glog/logging.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

std::unique_ptr<ifex::reference::DiscoverySyncBridge> g_bridge;
std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int signal) {
    LOG(INFO) << "Received signal " << signal << ", shutting down...";
    g_shutdown_requested.store(true);
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "\n"
              << "IFEX Discovery Sync Bridge - publishes Discovery state to cloud\n"
              << "\n"
              << "Options:\n"
              << "  --discovery=ENDPOINT      Discovery service endpoint (default: localhost:50051)\n"
              << "  --backend-transport=ENDPOINT\n"
              << "                            Backend Transport endpoint (default: localhost:50060)\n"
              << "  --content-id=ID           Content ID for sync messages (default: "
              << ifex::content_id::DISCOVERY_SYNC << ")\n"
              << "  --vehicle-id=ID           Vehicle identifier (default: vehicle-001)\n"
              << "  --init-delay=MS           Initialization delay (default: 5000)\n"
              << "  --poll-interval=MS        Polling interval (default: 1000)\n"
              << "  --batch-window=MS         Batch window, 0=immediate (default: 100)\n"
              << "  --heartbeat-interval=MS   Heartbeat interval, 0=disabled (default: 30000)\n"
              << "  --state-file=PATH         Path to persist sync state (optional)\n"
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
    ifex::reference::DiscoverySyncBridgeConfig config;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg.find("--discovery=") == 0) {
            config.discovery_endpoint = arg.substr(12);
        } else if (arg.find("--backend-transport=") == 0) {
            config.backend_transport_endpoint = arg.substr(20);
        } else if (arg.find("--content-id=") == 0) {
            config.sync_content_id = static_cast<uint32_t>(std::stoul(arg.substr(13)));
        } else if (arg.find("--vehicle-id=") == 0) {
            config.vehicle_id = arg.substr(13);
        } else if (arg.find("--init-delay=") == 0) {
            config.initialization_delay_ms = static_cast<uint32_t>(std::stoul(arg.substr(13)));
        } else if (arg.find("--poll-interval=") == 0) {
            config.poll_interval_ms = static_cast<uint32_t>(std::stoul(arg.substr(16)));
        } else if (arg.find("--batch-window=") == 0) {
            config.batch_window_ms = static_cast<uint32_t>(std::stoul(arg.substr(15)));
        } else if (arg.find("--heartbeat-interval=") == 0) {
            config.heartbeat_interval_ms = static_cast<uint32_t>(std::stoul(arg.substr(21)));
        } else if (arg.find("--state-file=") == 0) {
            config.state_persistence_path = arg.substr(13);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            LOG(ERROR) << "Unknown argument: " << arg;
            print_usage(argv[0]);
            return 1;
        }
    }

    LOG(INFO) << "Starting IFEX Discovery Sync Bridge";
    LOG(INFO) << "  Discovery endpoint: " << config.discovery_endpoint;
    LOG(INFO) << "  Backend Transport endpoint: " << config.backend_transport_endpoint;
    LOG(INFO) << "  Sync content_id: " << config.sync_content_id;
    LOG(INFO) << "  Vehicle ID: " << config.vehicle_id;
    LOG(INFO) << "  Initialization delay: " << config.initialization_delay_ms << "ms";
    LOG(INFO) << "  Poll interval: " << config.poll_interval_ms << "ms";
    LOG(INFO) << "  Batch window: " << config.batch_window_ms << "ms";
    LOG(INFO) << "  Heartbeat interval: " << config.heartbeat_interval_ms << "ms";

    try {
        // Create and start the bridge
        g_bridge = std::make_unique<ifex::reference::DiscoverySyncBridge>(config);

        if (!g_bridge->Start()) {
            LOG(ERROR) << "Failed to start Discovery Sync Bridge";
            return 1;
        }

        LOG(INFO) << "Discovery Sync Bridge is running. Press Ctrl+C to stop.";

        // Main loop - periodically log statistics
        auto last_stats_time = std::chrono::steady_clock::now();
        const auto stats_interval = std::chrono::seconds(60);

        while (!g_shutdown_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Log stats periodically
            auto now = std::chrono::steady_clock::now();
            if (now - last_stats_time >= stats_interval) {
                auto stats = g_bridge->GetStats();
                LOG(INFO) << "Stats: manifests=" << stats.manifests_sent
                          << " schema_responses=" << stats.schema_responses_sent
                          << " heartbeats=" << stats.heartbeats_sent
                          << " bytes=" << stats.bytes_sent
                          << " hashes=" << stats.hashes_tracked;
                last_stats_time = now;
            }
        }

        LOG(INFO) << "Shutdown requested, stopping bridge...";
        g_bridge->Stop();

        // Final stats
        auto stats = g_bridge->GetStats();
        LOG(INFO) << "Final stats: manifests=" << stats.manifests_sent
                  << " schema_responses=" << stats.schema_responses_sent
                  << " heartbeats=" << stats.heartbeats_sent
                  << " bytes=" << stats.bytes_sent;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Fatal error: " << e.what();
        return 1;
    }

    LOG(INFO) << "Discovery Sync Bridge stopped.";
    return 0;
}
