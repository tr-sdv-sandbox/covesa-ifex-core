#include "cloud_backend_transport_server.hpp"

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <memory>

// gRPC server settings
DEFINE_string(listen, "0.0.0.0:50100", "gRPC listen address");

// MQTT settings
DEFINE_string(mqtt_host, "localhost", "MQTT broker hostname");
DEFINE_int32(mqtt_port, 1883, "MQTT broker port");
DEFINE_string(mqtt_username, "", "MQTT username (optional)");
DEFINE_string(mqtt_password, "", "MQTT password (optional)");

// Partitioning (for horizontal scaling)
DEFINE_uint32(partition_id, 0, "Partition ID (0 for single partition)");
DEFINE_uint32(total_partitions, 1, "Total number of partitions");

// Topic prefixes
DEFINE_string(v2c_prefix, "v2c", "Vehicle-to-cloud topic prefix");
DEFINE_string(c2v_prefix, "c2v", "Cloud-to-vehicle topic prefix");

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

    // Configure server
    ifex::cloud::CloudBackendTransportServer::Config config;
    config.mqtt_host = FLAGS_mqtt_host;
    config.mqtt_port = FLAGS_mqtt_port;
    config.mqtt_username = FLAGS_mqtt_username;
    config.mqtt_password = FLAGS_mqtt_password;
    config.partition_id = FLAGS_partition_id;
    config.total_partitions = FLAGS_total_partitions;
    config.v2c_prefix = FLAGS_v2c_prefix;
    config.c2v_prefix = FLAGS_c2v_prefix;

    // Create transport server
    auto transport = std::make_unique<ifex::cloud::CloudBackendTransportServer>(config);

    // Start MQTT connection
    if (!transport->Start()) {
        LOG(ERROR) << "Failed to start transport server";
        return 1;
    }

    // Build gRPC server
    grpc::ServerBuilder builder;
    builder.AddListeningPort(FLAGS_listen, grpc::InsecureServerCredentials());

    // Register all services (server implements multiple service interfaces)
    builder.RegisterService(
        static_cast<swdv::cloud_backend_transport_service::send_to_vehicle_service::Service*>(transport.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_backend_transport_service::get_vehicle_status_service::Service*>(transport.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_backend_transport_service::get_channel_info_service::Service*>(transport.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_backend_transport_service::get_queue_status_service::Service*>(transport.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_backend_transport_service::get_stats_service::Service*>(transport.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_backend_transport_service::list_vehicles_service::Service*>(transport.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_backend_transport_service::healthy_service::Service*>(transport.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_backend_transport_service::on_vehicle_message_service::Service*>(transport.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_backend_transport_service::on_ack_service::Service*>(transport.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_backend_transport_service::on_vehicle_status_service::Service*>(transport.get()));
    builder.RegisterService(
        static_cast<swdv::cloud_backend_transport_service::on_queue_status_changed_service::Service*>(transport.get()));

    g_server = builder.BuildAndStart();
    if (!g_server) {
        LOG(ERROR) << "Failed to start gRPC server on " << FLAGS_listen;
        return 1;
    }

    LOG(INFO) << "CloudBackendTransportServer listening on " << FLAGS_listen;
    LOG(INFO) << "  partition=" << FLAGS_partition_id << "/" << FLAGS_total_partitions;
    LOG(INFO) << "  mqtt=" << FLAGS_mqtt_host << ":" << FLAGS_mqtt_port;
    LOG(INFO) << "  content_id routing: on-demand (clients specify in subscribe request)";

    // Block until shutdown
    g_server->Wait();

    LOG(INFO) << "Server stopped";
    transport->Stop();

    return 0;
}
