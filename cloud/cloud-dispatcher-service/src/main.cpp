#include "cloud_dispatcher_service.hpp"

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <memory>

// gRPC server settings
DEFINE_string(listen, "0.0.0.0:50104", "gRPC listen address");

// Transport settings
DEFINE_string(transport, "localhost:50100", "Cloud backend transport endpoint");
DEFINE_uint32(content_id, 200, "Content ID for dispatcher RPC");
DEFINE_uint32(default_timeout, 30000, "Default RPC timeout in milliseconds");

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
    google::SetStderrLogging(google::INFO);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // Install signal handlers
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // Configure service
    ifex::cloud::CloudDispatcherService::Config config;
    config.transport_endpoint = FLAGS_transport;
    config.content_id = FLAGS_content_id;
    config.default_timeout_ms = FLAGS_default_timeout;

    // Create dispatcher service
    auto dispatcher = std::make_unique<ifex::cloud::CloudDispatcherService>(config);

    // Start transport connection
    if (!dispatcher->Start()) {
        LOG(ERROR) << "Failed to start dispatcher service";
        return 1;
    }

    // Build gRPC server
    grpc::ServerBuilder builder;
    builder.AddListeningPort(FLAGS_listen, grpc::InsecureServerCredentials());

    // Register all services
    builder.RegisterService(
        static_cast<swdv::cloud_dispatcher_service::call_method_service::Service*>(dispatcher.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_dispatcher_service::call_method_async_service::Service*>(dispatcher.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_dispatcher_service::get_call_result_service::Service*>(dispatcher.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_dispatcher_service::list_pending_calls_service::Service*>(dispatcher.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_dispatcher_service::cancel_call_service::Service*>(dispatcher.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_dispatcher_service::healthy_service::Service*>(dispatcher.get()));

    g_server = builder.BuildAndStart();
    if (!g_server) {
        LOG(ERROR) << "Failed to start gRPC server on " << FLAGS_listen;
        return 1;
    }

    LOG(INFO) << "CloudDispatcherService listening on " << FLAGS_listen;
    LOG(INFO) << "  transport=" << FLAGS_transport;
    LOG(INFO) << "  content_id=" << FLAGS_content_id;
    LOG(INFO) << "  default_timeout=" << FLAGS_default_timeout << "ms";

    // Block until shutdown
    g_server->Wait();

    LOG(INFO) << "Server stopped";
    dispatcher->Stop();

    return 0;
}
