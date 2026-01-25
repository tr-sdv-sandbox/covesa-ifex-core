#include "cloud_scheduler_service.hpp"

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <memory>

DEFINE_string(listen, "0.0.0.0:50102", "Address to listen on for scheduler API");
DEFINE_string(transport, "localhost:50100", "Cloud backend transport address");
DEFINE_uint32(content_id, 202, "Scheduler sync content ID");

namespace {
std::unique_ptr<grpc::Server> g_server;
std::unique_ptr<ifex::cloud::CloudSchedulerService> g_service;

void SignalHandler(int signal) {
    LOG(INFO) << "Received signal " << signal << ", shutting down...";
    if (g_server) {
        g_server->Shutdown();
    }
    if (g_service) {
        g_service->Stop();
    }
}
}  // namespace

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_logtostderr = true;

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    LOG(INFO) << "IFEX Cloud Scheduler Service (in-memory)";
    LOG(INFO) << "  Listen:     " << FLAGS_listen;
    LOG(INFO) << "  Transport:  " << FLAGS_transport;
    LOG(INFO) << "  Content ID: " << FLAGS_content_id;

    ifex::cloud::CloudSchedulerServiceConfig config;
    config.backend_transport_address = FLAGS_transport;
    config.scheduler_content_id = FLAGS_content_id;

    g_service = std::make_unique<ifex::cloud::CloudSchedulerService>(config);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(FLAGS_listen, grpc::InsecureServerCredentials());
    g_service->RegisterServices(builder);

    g_server = builder.BuildAndStart();
    if (!g_server) {
        LOG(ERROR) << "Failed to start gRPC server";
        return 1;
    }

    if (!g_service->Start()) {
        LOG(ERROR) << "Failed to start scheduler service";
        g_server->Shutdown();
        return 1;
    }

    LOG(INFO) << "Cloud scheduler service running on " << FLAGS_listen;
    LOG(INFO) << "Press Ctrl+C to stop";
    g_server->Wait();

    LOG(INFO) << "Cloud scheduler service stopped";
    return 0;
}
