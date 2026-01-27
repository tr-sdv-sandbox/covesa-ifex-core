#include "cloud_discovery_service.hpp"

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <memory>

// gRPC server settings
DEFINE_string(listen, "0.0.0.0:50101", "gRPC listen address");

// Transport settings
DEFINE_string(transport, "localhost:50100", "Cloud backend transport endpoint");
DEFINE_uint32(content_id, 201, "Content ID for discovery sync");

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
    ifex::cloud::CloudDiscoveryService::Config config;
    config.transport_endpoint = FLAGS_transport;
    config.content_id = FLAGS_content_id;

    // Create discovery service
    auto discovery = std::make_unique<ifex::cloud::CloudDiscoveryService>(config);

    // Start transport connection
    if (!discovery->Start()) {
        LOG(ERROR) << "Failed to start discovery service";
        return 1;
    }

    // Build gRPC server
    grpc::ServerBuilder builder;
    builder.AddListeningPort(FLAGS_listen, grpc::InsecureServerCredentials());

    // Register all services
    builder.RegisterService(
        static_cast<swdv::cloud_discovery_service::get_vehicle_services_service::Service*>(discovery.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_discovery_service::find_services_service::Service*>(discovery.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_discovery_service::get_fleet_capabilities_service::Service*>(discovery.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_discovery_service::get_schema_service::Service*>(discovery.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_discovery_service::list_schemas_service::Service*>(discovery.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_discovery_service::get_vehicle_sync_status_service::Service*>(discovery.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_discovery_service::list_vehicles_service::Service*>(discovery.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_discovery_service::healthy_service::Service*>(discovery.get()));

    g_server = builder.BuildAndStart();
    if (!g_server) {
        LOG(ERROR) << "Failed to start gRPC server on " << FLAGS_listen;
        return 1;
    }

    LOG(INFO) << "CloudDiscoveryService listening on " << FLAGS_listen;
    LOG(INFO) << "  transport=" << FLAGS_transport;
    LOG(INFO) << "  content_id=" << FLAGS_content_id;

    // Block until shutdown
    g_server->Wait();

    LOG(INFO) << "Server stopped";
    discovery->Stop();

    return 0;
}
