#include "cloud_scheduler_sync_bridge.hpp"

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <memory>

DEFINE_string(listen, "0.0.0.0:50103", "Address to listen on for bridge API");
DEFINE_string(scheduler, "localhost:50102", "Cloud scheduler service address");
DEFINE_string(transport, "localhost:50100", "Cloud backend transport address");
DEFINE_uint32(content_id, 202, "Scheduler sync content ID");
DEFINE_string(instance_id, "", "Bridge instance ID (auto-generated if empty)");

namespace {
std::unique_ptr<grpc::Server> g_server;
std::unique_ptr<ifex::cloud::CloudSchedulerSyncBridge> g_bridge;

void SignalHandler(int signal) {
    LOG(INFO) << "Received signal " << signal << ", shutting down...";
    if (g_server) {
        g_server->Shutdown();
    }
    if (g_bridge) {
        g_bridge->Stop();
    }
}
}  // namespace

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_logtostderr = true;

    // Set up signal handlers
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    LOG(INFO) << "Starting cloud-scheduler-sync-bridge";
    LOG(INFO) << "  Listen:    " << FLAGS_listen;
    LOG(INFO) << "  Scheduler: " << FLAGS_scheduler;
    LOG(INFO) << "  Transport: " << FLAGS_transport;
    LOG(INFO) << "  Content ID: " << FLAGS_content_id;

    // Create bridge
    ifex::cloud::CloudSchedulerSyncBridgeConfig config;
    config.scheduler_address = FLAGS_scheduler;
    config.transport_address = FLAGS_transport;
    config.content_id = FLAGS_content_id;
    config.bridge_instance_id = FLAGS_instance_id;

    g_bridge = std::make_unique<ifex::cloud::CloudSchedulerSyncBridge>(config);

    // Start bridge
    if (!g_bridge->Start()) {
        LOG(ERROR) << "Failed to start sync bridge";
        return 1;
    }

    // Create gRPC server for bridge API
    grpc::ServerBuilder builder;
    builder.AddListeningPort(FLAGS_listen, grpc::InsecureServerCredentials());
    g_bridge->RegisterServices(builder);

    g_server = builder.BuildAndStart();
    if (!g_server) {
        LOG(ERROR) << "Failed to start gRPC server";
        g_bridge->Stop();
        return 1;
    }

    LOG(INFO) << "Cloud scheduler sync bridge running on " << FLAGS_listen;
    g_server->Wait();

    LOG(INFO) << "Cloud scheduler sync bridge stopped";
    return 0;
}
