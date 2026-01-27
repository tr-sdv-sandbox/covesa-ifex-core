#include "cloud_scheduler_service.hpp"

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <memory>

DEFINE_string(listen, "0.0.0.0:50102", "Address to listen on for scheduler API");

namespace {
std::unique_ptr<grpc::Server> g_server;

void SignalHandler(int signal) {
    LOG(INFO) << "Received signal " << signal << ", shutting down...";
    if (g_server) {
        g_server->Shutdown();
    }
}
}  // namespace

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_logtostderr = true;

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    LOG(INFO) << "IFEX Cloud Scheduler Service (in-memory storage)";
    LOG(INFO) << "  Listen: " << FLAGS_listen;
    LOG(INFO) << "  Note: This is pure storage. Use CloudSchedulerSyncBridge for vehicle sync.";

    // CloudSchedulerService is pure storage - no transport config needed
    ifex::cloud::CloudSchedulerServiceConfig config;
    auto service = std::make_unique<ifex::cloud::CloudSchedulerService>(config);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(FLAGS_listen, grpc::InsecureServerCredentials());
    service->RegisterServices(builder);

    g_server = builder.BuildAndStart();
    if (!g_server) {
        LOG(ERROR) << "Failed to start gRPC server";
        return 1;
    }

    LOG(INFO) << "Cloud scheduler service running on " << FLAGS_listen;
    LOG(INFO) << "Press Ctrl+C to stop";
    g_server->Wait();

    LOG(INFO) << "Cloud scheduler service stopped";
    return 0;
}
