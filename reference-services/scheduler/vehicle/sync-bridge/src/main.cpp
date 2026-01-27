/**
 * @file main.cpp
 * @brief Entry point for SchedulerSyncBridge service
 *
 * Synchronizes Scheduler job state to cloud via Backend Transport.
 */

#include "scheduler_sync_bridge.hpp"
#include "ifex_content_ids.hpp"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <chrono>
#include <csignal>
#include <thread>

DEFINE_string(scheduler, "localhost:50053",
              "Scheduler service endpoint");
DEFINE_string(backend_transport, "localhost:50060",
              "Backend Transport service endpoint");
DEFINE_string(vehicle_id, "vehicle-001",
              "Vehicle identifier for sync messages");
DEFINE_uint32(content_id, ifex::content_id::SCHEDULER_SYNC,
              "Content ID for scheduler sync (default: 202)");
DEFINE_uint32(init_delay_ms, 5000,
              "Initialization delay before first sync (ms)");
DEFINE_uint32(poll_interval_ms, 1000,
              "Polling interval for scheduler changes (ms)");
DEFINE_uint32(batch_window_ms, 100,
              "Event batching window (0 = immediate send)");
DEFINE_uint32(heartbeat_ms, 30000,
              "Heartbeat interval when no changes (0 = disabled)");
DEFINE_string(state_file, "",
              "Path to persist sync state (empty = no persistence)");
DEFINE_uint32(stats_interval_s, 60,
              "Interval for logging statistics (0 = disabled)");

namespace {
volatile std::sig_atomic_t g_shutdown_requested = 0;

void SignalHandler(int signal) {
    LOG(INFO) << "Received signal " << signal << ", shutting down...";
    g_shutdown_requested = 1;
}
}  // namespace

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    FLAGS_logtostderr = true;

    LOG(INFO) << "=== IFEX Scheduler Sync Bridge ===";
    LOG(INFO) << "Scheduler endpoint: " << FLAGS_scheduler;
    LOG(INFO) << "Backend Transport: " << FLAGS_backend_transport;
    LOG(INFO) << "Vehicle ID: " << FLAGS_vehicle_id;
    LOG(INFO) << "Content ID: " << FLAGS_content_id;

    // Set up signal handlers
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // Configure the bridge
    ifex::reference::SchedulerSyncBridgeConfig config;
    config.scheduler_endpoint = FLAGS_scheduler;
    config.backend_transport_endpoint = FLAGS_backend_transport;
    config.vehicle_id = FLAGS_vehicle_id;
    config.sync_content_id = FLAGS_content_id;
    config.initialization_delay_ms = FLAGS_init_delay_ms;
    config.poll_interval_ms = FLAGS_poll_interval_ms;
    config.batch_window_ms = FLAGS_batch_window_ms;
    config.heartbeat_interval_ms = FLAGS_heartbeat_ms;
    config.state_persistence_path = FLAGS_state_file;

    // Create and start the bridge
    ifex::reference::SchedulerSyncBridge bridge(config);

    if (!bridge.Start()) {
        LOG(ERROR) << "Failed to start SchedulerSyncBridge";
        return 1;
    }

    LOG(INFO) << "SchedulerSyncBridge running. Press Ctrl+C to stop.";

    // Main loop - periodically log stats
    auto last_stats_time = std::chrono::steady_clock::now();
    while (!g_shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        if (FLAGS_stats_interval_s > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_stats_time).count();

            if (elapsed >= FLAGS_stats_interval_s) {
                auto stats = bridge.GetStats();
                LOG(INFO) << "=== Scheduler Sync Stats ===";
                LOG(INFO) << "  Active jobs tracked: " << stats.active_jobs_tracked;
                LOG(INFO) << "  Events sent: " << stats.events_sent;
                LOG(INFO) << "  Full syncs: " << stats.full_syncs_sent;
                LOG(INFO) << "  Delta syncs: " << stats.delta_syncs_sent;
                LOG(INFO) << "  Execution results: " << stats.execution_results_sent;
                LOG(INFO) << "  Heartbeats: " << stats.heartbeats_sent;
                LOG(INFO) << "  Bytes sent: " << stats.bytes_sent;
                LOG(INFO) << "  Current sequence: " << stats.current_sequence;
                LOG(INFO) << "  Connected: " << (stats.is_connected ? "yes" : "no");
                last_stats_time = now;
            }
        }
    }

    LOG(INFO) << "Stopping SchedulerSyncBridge...";
    bridge.Stop();

    LOG(INFO) << "SchedulerSyncBridge stopped. Goodbye!";
    return 0;
}
