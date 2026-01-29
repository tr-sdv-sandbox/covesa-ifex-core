/**
 * @file test_scheduler_sync_bridge_e2e.cpp
 * @brief End-to-end integration tests for Scheduler Sync Bridge (vehicle + cloud)
 *
 * Tests the complete SchedulerSyncBridge component with REAL services on both sides:
 *
 * Vehicle side (from IntegrationTestFixture):
 * - Discovery service (service registry)
 * - Echo service (target for job invocation)
 * - Scheduler service (job storage) - we start our own in-process for sync testing
 * - BackendTransportServer (MQTT pub/sub)
 * - SchedulerSyncBridge (sync protocol)
 *
 * Cloud side:
 * - CloudSchedulerService (job storage)
 * - CloudBackendTransportServer (MQTT pub/sub)
 * - CloudSchedulerSyncBridge (sync protocol)
 *
 * Test scenarios:
 * 1. Basic sync: cloud→vehicle and vehicle→cloud job flow
 * 2. Bidirectional: jobs created on both sides simultaneously
 * 3. Offline modifications: changes while disconnected, reconcile on reconnect
 * 4. Conflict resolution: same job modified on both sides, authority wins
 * 5. Reconnection: state converges after disconnect/reconnect cycle
 */

#include "test_fixture.hpp"
#include <gtest/gtest.h>
#include <glog/logging.h>
#include <grpcpp/grpcpp.h>
#include <mosquitto.h>

// Vehicle side
#include "scheduler_server.hpp"
#include "backend_transport_server.hpp"
#include "scheduler_sync_bridge.hpp"

// Cloud side
#include "cloud_scheduler_service.hpp"
#include "cloud_backend_transport_server.hpp"
#include "cloud_scheduler_sync_bridge.hpp"

// Scheduler library (for hash-based comparison)
#include "job.hpp"
#include "job_hash.hpp"

// Proto
#include "scheduler-service.grpc.pb.h"
#include "cloud-scheduler-service.grpc.pb.h"
#include "scheduler-sync-v3.pb.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace ifex::test {

using namespace std::chrono_literals;

// Alias for scheduler library namespace
namespace sched = ifex::scheduler;

// =============================================================================
// Scheduler Sync Bridge E2E Test Fixture
// =============================================================================

class SchedulerSyncBridgeE2ETest : public IntegrationTestFixture {
protected:
    static constexpr uint32_t SYNC_CONTENT_ID = 202;
    static constexpr const char* TEST_VEHICLE_ID = "sync-bridge-e2e-test-vehicle";

    // Vehicle side - our own scheduler (separate from fixture's process-based scheduler)
    std::unique_ptr<reference::SchedulerServer> vehicle_scheduler_;
    std::unique_ptr<grpc::Server> vehicle_scheduler_grpc_;
    int vehicle_scheduler_port_ = 0;

    std::unique_ptr<reference::BackendTransportServer> vehicle_transport_;
    std::unique_ptr<grpc::Server> vehicle_transport_grpc_;
    int vehicle_transport_port_ = 0;

    std::unique_ptr<reference::SchedulerSyncBridge> vehicle_sync_bridge_;

    // Cloud side
    std::unique_ptr<cloud::CloudSchedulerService> cloud_scheduler_;
    std::unique_ptr<grpc::Server> cloud_scheduler_grpc_;
    int cloud_scheduler_port_ = 0;

    std::unique_ptr<cloud::CloudBackendTransportServer> cloud_transport_;
    std::unique_ptr<grpc::Server> cloud_transport_grpc_;
    int cloud_transport_port_ = 0;

    std::unique_ptr<cloud::CloudSchedulerSyncBridge> cloud_sync_bridge_;
    std::unique_ptr<grpc::Server> cloud_sync_bridge_grpc_;
    int cloud_sync_bridge_port_ = 0;

    void SetUp() override {
        IntegrationTestFixture::SetUp();

        // Check MQTT is available from fixture
        if (!IsMqttAvailable()) {
            GTEST_SKIP() << "MQTT not available - skipping sync bridge E2E tests";
            return;
        }

        // Start cloud side first (it listens for vehicle connections)
        ASSERT_TRUE(StartCloudServices());

        // Start vehicle side (using fixture's Discovery for service validation)
        ASSERT_TRUE(StartVehicleServices());

        // Wait for services to establish connections
        std::this_thread::sleep_for(2s);
        LOG(INFO) << "Scheduler Sync Bridge E2E test environment ready";
    }

    void TearDown() override {
        StopVehicleSyncBridge();
        StopCloudSyncBridge();
        std::this_thread::sleep_for(200ms);

        StopVehicleServices();
        StopCloudServices();

        IntegrationTestFixture::TearDown();
    }

    // =========================================================================
    // Service Management
    // =========================================================================

    bool StartCloudServices() {
        // Cloud transport
        cloud::CloudBackendTransportServer::Config transport_config;
        transport_config.mqtt_host = GetMqttHost();
        transport_config.mqtt_port = GetMqttPort();
        transport_config.partition_id = 0;
        transport_config.total_partitions = 1;

        cloud_transport_ = std::make_unique<cloud::CloudBackendTransportServer>(transport_config);
        if (!cloud_transport_->Start()) {
            LOG(ERROR) << "Failed to start cloud transport";
            return false;
        }

        {
            grpc::ServerBuilder builder;
            builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &cloud_transport_port_);
            using namespace swdv::cloud_backend_transport_service;
            builder.RegisterService(static_cast<send_to_vehicle_service::Service*>(cloud_transport_.get()));
            builder.RegisterService(static_cast<healthy_service::Service*>(cloud_transport_.get()));
            builder.RegisterService(static_cast<on_vehicle_message_service::Service*>(cloud_transport_.get()));
            cloud_transport_grpc_ = builder.BuildAndStart();
        }
        LOG(INFO) << "Cloud transport on port " << cloud_transport_port_;

        // Cloud scheduler (pure storage - sync bridge handles backend communication)
        cloud::CloudSchedulerServiceConfig sched_config;
        cloud_scheduler_ = std::make_unique<cloud::CloudSchedulerService>(sched_config);

        {
            grpc::ServerBuilder builder;
            builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &cloud_scheduler_port_);
            cloud_scheduler_->RegisterServices(builder);
            cloud_scheduler_grpc_ = builder.BuildAndStart();
        }
        LOG(INFO) << "Cloud scheduler on port " << cloud_scheduler_port_;

        return true;
    }

    bool StartVehicleServices() {
        // Vehicle scheduler - use fixture's Discovery for service validation
        reference::SchedulerServer::Config sched_config;
        sched_config.discovery_endpoint = TEST_DISCOVERY_ADDRESS;
        // In-memory, no persistence (empty persistence_dir)

        vehicle_scheduler_ = std::make_unique<reference::SchedulerServer>(sched_config);

        {
            grpc::ServerBuilder builder;
            builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &vehicle_scheduler_port_);
            // Register all scheduler service interfaces
            using namespace swdv::ifex_scheduler;
            builder.RegisterService(static_cast<create_job_service::Service*>(vehicle_scheduler_.get()));
            builder.RegisterService(static_cast<list_jobs_service::Service*>(vehicle_scheduler_.get()));
            builder.RegisterService(static_cast<get_job_service::Service*>(vehicle_scheduler_.get()));
            builder.RegisterService(static_cast<update_job_service::Service*>(vehicle_scheduler_.get()));
            builder.RegisterService(static_cast<delete_job_service::Service*>(vehicle_scheduler_.get()));
            builder.RegisterService(static_cast<pause_job_service::Service*>(vehicle_scheduler_.get()));
            builder.RegisterService(static_cast<resume_job_service::Service*>(vehicle_scheduler_.get()));
            builder.RegisterService(static_cast<trigger_job_service::Service*>(vehicle_scheduler_.get()));
            vehicle_scheduler_grpc_ = builder.BuildAndStart();
        }
        LOG(INFO) << "Vehicle scheduler on port " << vehicle_scheduler_port_;

        // Vehicle transport
        reference::BackendTransportServer::Config transport_config;
        transport_config.mqtt_host = GetMqttHost();
        transport_config.mqtt_port = GetMqttPort();
        transport_config.vehicle_id = TEST_VEHICLE_ID;
        transport_config.persistence_dir = "/tmp/ifex-scheduler-sync-bridge-e2e-vehicle";

        vehicle_transport_ = std::make_unique<reference::BackendTransportServer>(transport_config);
        if (!vehicle_transport_->Start()) {
            LOG(ERROR) << "Failed to start vehicle transport";
            return false;
        }

        {
            grpc::ServerBuilder builder;
            builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &vehicle_transport_port_);
            using namespace swdv::backend_transport_service;
            builder.RegisterService(static_cast<publish_service::Service*>(vehicle_transport_.get()));
            builder.RegisterService(static_cast<healthy_service::Service*>(vehicle_transport_.get()));
            builder.RegisterService(static_cast<on_content_service::Service*>(vehicle_transport_.get()));
            vehicle_transport_grpc_ = builder.BuildAndStart();
        }
        LOG(INFO) << "Vehicle transport on port " << vehicle_transport_port_;

        return true;
    }

    void StartVehicleSyncBridge() {
        reference::SchedulerSyncBridgeConfig config;
        config.scheduler_endpoint = "localhost:" + std::to_string(vehicle_scheduler_port_);
        config.backend_transport_endpoint = "localhost:" + std::to_string(vehicle_transport_port_);
        config.sync_content_id = SYNC_CONTENT_ID;
        config.vehicle_id = TEST_VEHICLE_ID;
        config.initialization_delay_ms = 500;  // Short delay for tests
        config.poll_interval_ms = 500;
        config.heartbeat_interval_ms = 5000;

        vehicle_sync_bridge_ = std::make_unique<reference::SchedulerSyncBridge>(config);
        ASSERT_TRUE(vehicle_sync_bridge_->Start());
        LOG(INFO) << "Vehicle sync bridge started";
    }

    void StartCloudSyncBridge() {
        cloud::CloudSchedulerSyncBridgeConfig config;
        config.scheduler_address = "localhost:" + std::to_string(cloud_scheduler_port_);
        config.transport_address = "localhost:" + std::to_string(cloud_transport_port_);
        config.content_id = SYNC_CONTENT_ID;

        cloud_sync_bridge_ = std::make_unique<cloud::CloudSchedulerSyncBridge>(config);

        {
            grpc::ServerBuilder builder;
            builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &cloud_sync_bridge_port_);
            cloud_sync_bridge_->RegisterServices(builder);
            cloud_sync_bridge_grpc_ = builder.BuildAndStart();
        }

        ASSERT_TRUE(cloud_sync_bridge_->Start());
        LOG(INFO) << "Cloud sync bridge started on port " << cloud_sync_bridge_port_;
    }

    void StopVehicleSyncBridge() {
        if (vehicle_sync_bridge_) {
            vehicle_sync_bridge_->Stop();
            vehicle_sync_bridge_.reset();
        }
    }

    void StopCloudSyncBridge() {
        if (cloud_sync_bridge_) {
            cloud_sync_bridge_->Stop();
        }
        if (cloud_sync_bridge_grpc_) {
            cloud_sync_bridge_grpc_->Shutdown();
            cloud_sync_bridge_grpc_.reset();
        }
        cloud_sync_bridge_.reset();
    }

    void StopVehicleServices() {
        if (vehicle_transport_) vehicle_transport_->Stop();
        if (vehicle_transport_grpc_) {
            vehicle_transport_grpc_->Shutdown();
            vehicle_transport_grpc_.reset();
        }
        vehicle_transport_.reset();

        if (vehicle_scheduler_grpc_) {
            vehicle_scheduler_grpc_->Shutdown();
            vehicle_scheduler_grpc_.reset();
        }
        vehicle_scheduler_.reset();
    }

    void StopCloudServices() {
        // CloudSchedulerService has no lifecycle - just shutdown gRPC and reset
        if (cloud_scheduler_grpc_) {
            cloud_scheduler_grpc_->Shutdown();
            cloud_scheduler_grpc_.reset();
        }
        cloud_scheduler_.reset();

        if (cloud_transport_) cloud_transport_->Stop();
        if (cloud_transport_grpc_) {
            cloud_transport_grpc_->Shutdown();
            cloud_transport_grpc_.reset();
        }
        cloud_transport_.reset();
    }

    // =========================================================================
    // Job Conversion and Comparison using Library Functions
    // =========================================================================

    /// Convert proto job to library Job for hash comparison
    /// Both cloud and vehicle now use the common scheduler_types::job_t
    static sched::Job ProtoJobToLibraryJob(const swdv::scheduler_types::job_t& proto) {
        sched::Job job;
        job.job_id = proto.job_id();
        job.title = proto.title();
        job.service = proto.service();
        job.method = proto.method();
        job.parameters_json = proto.parameters_json();
        job.scheduled_time_ms = proto.scheduled_time_ms();
        job.recurrence_rule = proto.recurrence_rule();
        job.end_time_ms = proto.end_time_ms();
        job.paused = proto.paused();
        job.deleted = proto.deleted();
        // wake_policy, sleep_policy, wake_lead_time_s
        job.wake_policy = static_cast<sched::WakePolicy>(proto.wake_policy());
        job.sleep_policy = static_cast<sched::SleepPolicy>(proto.sleep_policy());
        job.wake_lead_time_s = proto.wake_lead_time_s();
        // Sync state fields for checksum (per spec section 5.5)
        job.local_version.cloud_seq = proto.local_version().cloud_seq();
        job.local_version.vehicle_seq = proto.local_version().vehicle_seq();
        job.authority = static_cast<sched::JobAuthority>(proto.authority());
        return job;
    }

    /// Debug string for a library Job
    static std::string JobDebugString(const sched::Job& job) {
        std::ostringstream oss;
        oss << "Job{id=" << job.job_id
            << ", title=" << job.title
            << ", service=" << job.service
            << ", method=" << job.method
            << ", params=" << job.parameters_json
            << ", scheduled=" << job.scheduled_time_ms
            << ", paused=" << job.paused
            << ", deleted=" << job.deleted
            << ", hash=" << std::hex << job.content_hash() << std::dec << "}";
        return oss.str();
    }

    // =========================================================================
    // Helper Methods
    // =========================================================================

    /// Create a job on the cloud scheduler
    std::string CreateCloudJob(const std::string& title,
                               uint64_t scheduled_time_ms = 4102444799000ULL) {
        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(cloud_scheduler_port_),
            grpc::InsecureChannelCredentials());
        auto stub = swdv::cloud_scheduler_service::create_job_service::NewStub(channel);

        grpc::ClientContext context;
        swdv::cloud_scheduler_service::create_job_request request;
        auto* req = request.mutable_request();
        req->set_vehicle_id(TEST_VEHICLE_ID);
        req->set_title(title);
        req->set_service("echo_service");
        req->set_method("echo");
        req->set_parameters_json(R"({"message": "test"})");
        req->set_scheduled_time_ms(scheduled_time_ms);

        swdv::cloud_scheduler_service::create_job_response response;
        auto status = stub->create_job(&context, request, &response);

        if (!status.ok() || !response.result().success()) {
            LOG(ERROR) << "Failed to create cloud job: "
                       << (status.ok() ? response.result().error_message() : status.error_message());
            return "";
        }
        LOG(INFO) << "Created cloud job: " << title << " -> " << response.result().job_id();
        return response.result().job_id();
    }

    /// Create a job on the vehicle scheduler
    std::string CreateVehicleJob(const std::string& title,
                                 uint64_t scheduled_time_ms = 4102444799000ULL) {
        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(vehicle_scheduler_port_),
            grpc::InsecureChannelCredentials());
        auto stub = swdv::ifex_scheduler::create_job_service::NewStub(channel);

        grpc::ClientContext context;
        swdv::ifex_scheduler::create_job_request request;
        auto* job = request.mutable_job();
        job->set_title(title);
        job->set_service("echo_service");
        job->set_method("echo");
        job->set_parameters_json(R"({"message": "test"})");
        job->set_scheduled_time_ms(scheduled_time_ms);

        swdv::ifex_scheduler::create_job_response response;
        auto status = stub->create_job(&context, request, &response);

        if (!status.ok() || !response.success()) {
            LOG(ERROR) << "Failed to create vehicle job: "
                       << (status.ok() ? response.message() : status.error_message());
            return "";
        }
        LOG(INFO) << "Created vehicle job: " << title << " -> " << response.job_id();
        return response.job_id();
    }

    /// Update a job on the cloud scheduler
    bool UpdateCloudJob(const std::string& job_id, const std::string& new_title,
                        uint64_t new_scheduled_time_ms = 0) {
        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(cloud_scheduler_port_),
            grpc::InsecureChannelCredentials());
        auto stub = swdv::cloud_scheduler_service::update_job_service::NewStub(channel);

        grpc::ClientContext context;
        swdv::cloud_scheduler_service::update_job_request request;
        auto* req = request.mutable_request();
        req->set_vehicle_id(TEST_VEHICLE_ID);
        req->set_job_id(job_id);
        req->set_title(new_title);
        if (new_scheduled_time_ms > 0) {
            req->set_scheduled_time_ms(new_scheduled_time_ms);
        }

        swdv::cloud_scheduler_service::update_job_response response;
        auto status = stub->update_job(&context, request, &response);
        return status.ok() && response.result().success();
    }

    /// Update a job on the vehicle scheduler
    bool UpdateVehicleJob(const std::string& job_id, const std::string& new_title,
                          uint64_t new_scheduled_time_ms = 0) {
        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(vehicle_scheduler_port_),
            grpc::InsecureChannelCredentials());
        auto stub = swdv::ifex_scheduler::update_job_service::NewStub(channel);

        grpc::ClientContext context;
        swdv::ifex_scheduler::update_job_request request;
        request.set_job_id(job_id);
        auto* updates = request.mutable_updates();
        updates->set_title(new_title);
        if (new_scheduled_time_ms > 0) {
            updates->set_scheduled_time_ms(new_scheduled_time_ms);
        }

        swdv::ifex_scheduler::update_job_response response;
        auto status = stub->update_job(&context, request, &response);
        return status.ok() && response.success();
    }

    /// Delete a job on the cloud scheduler
    bool DeleteCloudJob(const std::string& job_id) {
        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(cloud_scheduler_port_),
            grpc::InsecureChannelCredentials());
        auto stub = swdv::cloud_scheduler_service::delete_job_service::NewStub(channel);

        grpc::ClientContext context;
        swdv::cloud_scheduler_service::delete_job_request request;
        request.set_vehicle_id(TEST_VEHICLE_ID);
        request.set_job_id(job_id);

        swdv::cloud_scheduler_service::delete_job_response response;
        auto status = stub->delete_job(&context, request, &response);
        return status.ok() && response.result().success();
    }

    /// Delete a job on the vehicle scheduler
    bool DeleteVehicleJob(const std::string& job_id) {
        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(vehicle_scheduler_port_),
            grpc::InsecureChannelCredentials());
        auto stub = swdv::ifex_scheduler::delete_job_service::NewStub(channel);

        grpc::ClientContext context;
        swdv::ifex_scheduler::delete_job_request request;
        request.set_job_id(job_id);

        swdv::ifex_scheduler::delete_job_response response;
        auto status = stub->delete_job(&context, request, &response);
        return status.ok() && response.success();
    }

    /// Pause a job on the cloud scheduler
    bool PauseCloudJob(const std::string& job_id) {
        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(cloud_scheduler_port_),
            grpc::InsecureChannelCredentials());
        auto stub = swdv::cloud_scheduler_service::pause_job_service::NewStub(channel);

        grpc::ClientContext context;
        swdv::cloud_scheduler_service::pause_job_request request;
        request.set_vehicle_id(TEST_VEHICLE_ID);
        request.set_job_id(job_id);

        swdv::cloud_scheduler_service::pause_job_response response;
        auto status = stub->pause_job(&context, request, &response);
        return status.ok() && response.result().success();
    }

    /// Pause a job on the vehicle scheduler
    bool PauseVehicleJob(const std::string& job_id) {
        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(vehicle_scheduler_port_),
            grpc::InsecureChannelCredentials());
        auto stub = swdv::ifex_scheduler::pause_job_service::NewStub(channel);

        grpc::ClientContext context;
        swdv::ifex_scheduler::pause_job_request request;
        request.set_job_id(job_id);

        swdv::ifex_scheduler::pause_job_response response;
        auto status = stub->pause_job(&context, request, &response);
        return status.ok() && response.success();
    }

    /// Get all jobs from cloud scheduler as library Jobs (sorted by job_id)
    std::vector<sched::Job> GetAllCloudJobs() {
        std::vector<sched::Job> result;

        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(cloud_scheduler_port_),
            grpc::InsecureChannelCredentials());
        auto stub = swdv::cloud_scheduler_service::get_jobs_for_vehicle_service::NewStub(channel);

        grpc::ClientContext context;
        swdv::cloud_scheduler_service::get_jobs_for_vehicle_request request;
        request.set_vehicle_id(TEST_VEHICLE_ID);
        request.set_include_deleted(true);  // Include tombstones for comparison
        swdv::cloud_scheduler_service::get_jobs_for_vehicle_response response;

        auto status = stub->get_jobs_for_vehicle(&context, request, &response);
        if (!status.ok()) {
            LOG(ERROR) << "Failed to get cloud jobs: " << status.error_message();
            return result;
        }

        for (const auto& job : response.jobs()) {
            result.push_back(ProtoJobToLibraryJob(job));
        }

        // Sort by job_id for deterministic checksum
        std::sort(result.begin(), result.end(),
                  [](const sched::Job& a, const sched::Job& b) { return a.job_id < b.job_id; });

        return result;
    }

    /// Get all jobs from vehicle scheduler as library Jobs (sorted by job_id)
    /// Sets include_deleted=true to get all jobs including tombstones
    std::vector<sched::Job> GetAllVehicleJobs() {
        std::vector<sched::Job> result;

        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(vehicle_scheduler_port_),
            grpc::InsecureChannelCredentials());
        auto stub = swdv::ifex_scheduler::list_jobs_service::NewStub(channel);

        grpc::ClientContext context;
        swdv::ifex_scheduler::list_jobs_request request;
        // Include tombstones for sync testing
        request.mutable_filter()->set_include_deleted(true);
        swdv::ifex_scheduler::list_jobs_response response;

        auto status = stub->list_jobs(&context, request, &response);
        if (!status.ok() || !response.success()) {
            LOG(ERROR) << "Failed to get vehicle jobs: " << status.error_message();
            return result;
        }

        for (const auto& proto_job : response.jobs()) {
            result.push_back(ProtoJobToLibraryJob(proto_job));
        }

        // Sort by job_id for deterministic checksum
        std::sort(result.begin(), result.end(),
                  [](const sched::Job& a, const sched::Job& b) { return a.job_id < b.job_id; });

        return result;
    }

    /// Verify that cloud and vehicle have converged to the same state
    /// Uses the same hash-based comparison as the sync protocol
    /// If verbose=false, only returns result without logging (for polling)
    bool VerifyConvergence(const std::string& context_msg = "", bool verbose = true) {
        auto cloud_jobs = GetAllCloudJobs();
        auto vehicle_jobs = GetAllVehicleJobs();

        // Use the library's state checksum for comparison
        // This is exactly what the sync protocol uses for quiescence detection
        uint64_t cloud_checksum = sched::compute_state_checksum(cloud_jobs);
        uint64_t vehicle_checksum = sched::compute_state_checksum(vehicle_jobs);

        if (cloud_checksum == vehicle_checksum) {
            if (verbose) {
                LOG(INFO) << context_msg << " States converged: checksum=" << std::hex << cloud_checksum
                          << std::dec << " (" << cloud_jobs.size() << " jobs)";
            }
            return true;
        }

        if (!verbose) {
            return false;  // Silent check for polling
        }

        // Checksums differ - provide detailed diff for debugging
        LOG(ERROR) << context_msg << " States differ: cloud_checksum=" << std::hex << cloud_checksum
                   << " vehicle_checksum=" << vehicle_checksum << std::dec;
        LOG(ERROR) << context_msg << " Cloud has " << cloud_jobs.size() << " jobs, vehicle has "
                   << vehicle_jobs.size() << " jobs";

        // Build maps for easier comparison
        std::map<std::string, sched::Job> cloud_map, vehicle_map;
        for (const auto& j : cloud_jobs) cloud_map[j.job_id] = j;
        for (const auto& j : vehicle_jobs) vehicle_map[j.job_id] = j;

        // Find differences
        for (const auto& [id, cloud_job] : cloud_map) {
            auto it = vehicle_map.find(id);
            if (it == vehicle_map.end()) {
                LOG(ERROR) << context_msg << " Job " << id << " on cloud only: "
                           << JobDebugString(cloud_job);
            } else if (!cloud_job.content_equals(it->second)) {
                LOG(ERROR) << context_msg << " Job " << id << " hash mismatch:";
                LOG(ERROR) << "  Cloud:   " << JobDebugString(cloud_job);
                LOG(ERROR) << "  Vehicle: " << JobDebugString(it->second);
            }
        }

        for (const auto& [id, vehicle_job] : vehicle_map) {
            if (cloud_map.find(id) == cloud_map.end()) {
                LOG(ERROR) << context_msg << " Job " << id << " on vehicle only: "
                           << JobDebugString(vehicle_job);
            }
        }

        return false;
    }

    /// Wait for convergence with timeout (polls silently, logs details only on final check)
    bool WaitForConvergence(std::chrono::seconds timeout = 15s,
                            const std::string& context_msg = "") {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (VerifyConvergence(context_msg, false)) {  // Silent polling
                // Log success
                VerifyConvergence(context_msg, true);
                return true;
            }
            std::this_thread::sleep_for(200ms);
        }
        // Final verbose check to log what went wrong
        return VerifyConvergence(context_msg, true);
    }

    /// Wait for condition with timeout
    template<typename Predicate>
    bool WaitFor(Predicate pred, std::chrono::seconds timeout = 10s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) return true;
            std::this_thread::sleep_for(200ms);
        }
        return false;
    }

    /// Check if job exists on cloud (and is not deleted)
    bool CloudHasJob(const std::string& job_id) {
        auto jobs = GetAllCloudJobs();
        for (const auto& job : jobs) {
            if (job.job_id == job_id && !job.deleted) return true;
        }
        return false;
    }

    /// Check if job exists on vehicle (and is not deleted)
    bool VehicleHasJob(const std::string& job_id) {
        auto jobs = GetAllVehicleJobs();
        for (const auto& job : jobs) {
            if (job.job_id == job_id && !job.deleted) return true;
        }
        return false;
    }

    /// Get job count on cloud (non-deleted only)
    size_t GetCloudJobCount() {
        auto jobs = GetAllCloudJobs();
        return std::count_if(jobs.begin(), jobs.end(),
                             [](const sched::Job& j) { return !j.deleted; });
    }

    /// Get job count on vehicle (non-deleted only)
    size_t GetVehicleJobCount() {
        auto jobs = GetAllVehicleJobs();
        return std::count_if(jobs.begin(), jobs.end(),
                             [](const sched::Job& j) { return !j.deleted; });
    }

    /// Find a job by ID in a vector of jobs
    static const sched::Job* FindJobById(const std::vector<sched::Job>& jobs,
                                         const std::string& job_id) {
        for (const auto& job : jobs) {
            if (job.job_id == job_id) return &job;
        }
        return nullptr;
    }

    /// Check if a job is deleted (by ID)
    bool IsJobDeleted(const std::vector<sched::Job>& jobs, const std::string& job_id) {
        auto* job = FindJobById(jobs, job_id);
        return job && job->deleted;
    }

    /// Check if a job is paused (by ID)
    bool IsJobPaused(const std::vector<sched::Job>& jobs, const std::string& job_id) {
        auto* job = FindJobById(jobs, job_id);
        return job && job->paused;
    }

    /// Get job title (by ID)
    std::string GetJobTitle(const std::vector<sched::Job>& jobs, const std::string& job_id) {
        auto* job = FindJobById(jobs, job_id);
        return job ? job->title : "";
    }
};

// =============================================================================
// Test Cases - State Convergence Verification
// =============================================================================

/// Test: Basic cloud-to-vehicle sync with state convergence verification
TEST_F(SchedulerSyncBridgeE2ETest, CloudJobSyncsToVehicle_StateConverges) {
    StartCloudSyncBridge();
    StartVehicleSyncBridge();
    EXPECT_TRUE(WaitFor([&]() { return vehicle_sync_bridge_->IsInitialized(); }, 10s));

    // Create jobs on cloud
    std::string job1 = CreateCloudJob("Cloud Job 1");
    std::string job2 = CreateCloudJob("Cloud Job 2");
    ASSERT_FALSE(job1.empty());
    ASSERT_FALSE(job2.empty());

    // Wait for FULL convergence - not just "job exists" but identical state
    EXPECT_TRUE(WaitForConvergence(20s, "[CloudToVehicle]"))
        << "Cloud and vehicle should have identical state after sync";

    // Double-check: verify specific fields match
    auto cloud_jobs = GetAllCloudJobs();
    auto vehicle_jobs = GetAllVehicleJobs();
    EXPECT_EQ(cloud_jobs.size(), vehicle_jobs.size());
    auto* cloud_job1 = FindJobById(cloud_jobs, job1);
    auto* vehicle_job1 = FindJobById(vehicle_jobs, job1);
    ASSERT_NE(cloud_job1, nullptr);
    ASSERT_NE(vehicle_job1, nullptr);
    EXPECT_EQ(cloud_job1->title, vehicle_job1->title);
    EXPECT_EQ(cloud_job1->scheduled_time_ms, vehicle_job1->scheduled_time_ms);
}

/// Test: Basic vehicle-to-cloud sync with state convergence verification
TEST_F(SchedulerSyncBridgeE2ETest, VehicleJobSyncsToCloud_StateConverges) {
    StartCloudSyncBridge();
    StartVehicleSyncBridge();
    EXPECT_TRUE(WaitFor([&]() { return vehicle_sync_bridge_->IsInitialized(); }, 10s));

    // Create jobs on vehicle
    std::string job1 = CreateVehicleJob("Vehicle Job 1");
    std::string job2 = CreateVehicleJob("Vehicle Job 2");
    ASSERT_FALSE(job1.empty());
    ASSERT_FALSE(job2.empty());

    // Wait for FULL convergence
    EXPECT_TRUE(WaitForConvergence(20s, "[VehicleToCloud]"))
        << "Cloud and vehicle should have identical state after sync";
}

/// Test: Bidirectional sync - jobs created on both sides simultaneously
TEST_F(SchedulerSyncBridgeE2ETest, BidirectionalSync_StateConverges) {
    StartCloudSyncBridge();
    StartVehicleSyncBridge();
    EXPECT_TRUE(WaitFor([&]() { return vehicle_sync_bridge_->IsInitialized(); }, 10s));

    // Create jobs on BOTH sides at roughly the same time
    std::string cloud_job1 = CreateCloudJob("Bidir Cloud Job 1");
    std::string vehicle_job1 = CreateVehicleJob("Bidir Vehicle Job 1");
    std::string cloud_job2 = CreateCloudJob("Bidir Cloud Job 2");
    std::string vehicle_job2 = CreateVehicleJob("Bidir Vehicle Job 2");

    ASSERT_FALSE(cloud_job1.empty());
    ASSERT_FALSE(vehicle_job1.empty());
    ASSERT_FALSE(cloud_job2.empty());
    ASSERT_FALSE(vehicle_job2.empty());

    // Wait for convergence - all 4 jobs should exist on both sides with identical state
    EXPECT_TRUE(WaitForConvergence(25s, "[Bidirectional]"))
        << "All jobs should sync bidirectionally with identical state";

    // Verify count
    EXPECT_EQ(GetAllCloudJobs().size(), 4u);
    EXPECT_EQ(GetAllVehicleJobs().size(), 4u);
}

/// Test: Offline modifications - create jobs while disconnected, verify convergence on reconnect
TEST_F(SchedulerSyncBridgeE2ETest, OfflineCreation_ConvergesOnReconnect) {
    // Start connected and create initial state
    StartCloudSyncBridge();
    StartVehicleSyncBridge();
    EXPECT_TRUE(WaitFor([&]() { return vehicle_sync_bridge_->IsInitialized(); }, 10s));

    std::string initial_job = CreateCloudJob("Initial Job");
    ASSERT_FALSE(initial_job.empty());
    EXPECT_TRUE(WaitForConvergence(15s, "[Initial]"));

    LOG(INFO) << "=== Going offline ===";
    StopVehicleSyncBridge();
    StopCloudSyncBridge();
    std::this_thread::sleep_for(500ms);

    // Create jobs on BOTH sides while "offline"
    std::string offline_cloud_job = CreateCloudJob("Offline Cloud Job");
    std::string offline_vehicle_job = CreateVehicleJob("Offline Vehicle Job");
    ASSERT_FALSE(offline_cloud_job.empty());
    ASSERT_FALSE(offline_vehicle_job.empty());

    // Verify they haven't synced (sanity check)
    EXPECT_FALSE(VehicleHasJob(offline_cloud_job));
    EXPECT_FALSE(CloudHasJob(offline_vehicle_job));

    LOG(INFO) << "=== Reconnecting ===";
    StartCloudSyncBridge();
    StartVehicleSyncBridge();

    // After reconnect, BOTH offline jobs should exist on BOTH sides
    EXPECT_TRUE(WaitForConvergence(25s, "[AfterReconnect]"))
        << "All jobs (initial + offline from both sides) should converge";

    // Verify all 3 jobs exist
    auto cloud_jobs = GetAllCloudJobs();
    auto vehicle_jobs = GetAllVehicleJobs();
    EXPECT_EQ(cloud_jobs.size(), 3u);
    EXPECT_EQ(vehicle_jobs.size(), 3u);
    EXPECT_TRUE(FindJobById(cloud_jobs, initial_job) != nullptr);
    EXPECT_TRUE(FindJobById(cloud_jobs, offline_cloud_job) != nullptr);
    EXPECT_TRUE(FindJobById(cloud_jobs, offline_vehicle_job) != nullptr);
}

/// Test: Offline modifications to EXISTING jobs - update same job on both sides
TEST_F(SchedulerSyncBridgeE2ETest, OfflineModification_ConflictResolution) {
    // Create and sync a job
    StartCloudSyncBridge();
    StartVehicleSyncBridge();
    EXPECT_TRUE(WaitFor([&]() { return vehicle_sync_bridge_->IsInitialized(); }, 10s));

    std::string shared_job = CreateCloudJob("Shared Job Original Title");
    ASSERT_FALSE(shared_job.empty());
    EXPECT_TRUE(WaitForConvergence(15s, "[Initial]"));

    LOG(INFO) << "=== Going offline ===";
    StopVehicleSyncBridge();
    StopCloudSyncBridge();
    std::this_thread::sleep_for(500ms);

    // Modify the SAME job on BOTH sides while offline (conflict!)
    EXPECT_TRUE(UpdateCloudJob(shared_job, "Cloud Modified Title"));
    EXPECT_TRUE(UpdateVehicleJob(shared_job, "Vehicle Modified Title"));

    // Verify each side sees its own modification
    auto cloud_before = GetAllCloudJobs();
    auto vehicle_before = GetAllVehicleJobs();
    EXPECT_EQ(GetJobTitle(cloud_before, shared_job), "Cloud Modified Title");
    EXPECT_EQ(GetJobTitle(vehicle_before, shared_job), "Vehicle Modified Title");

    LOG(INFO) << "=== Reconnecting - conflict resolution should occur ===";
    StartCloudSyncBridge();
    StartVehicleSyncBridge();

    // Wait for convergence - ONE winner should emerge based on authority
    EXPECT_TRUE(WaitForConvergence(25s, "[AfterConflict]"))
        << "Conflict should resolve to single consistent state";

    // Verify both sides have the SAME title (whichever won)
    auto cloud_after = GetAllCloudJobs();
    auto vehicle_after = GetAllVehicleJobs();
    EXPECT_EQ(GetJobTitle(cloud_after, shared_job), GetJobTitle(vehicle_after, shared_job))
        << "Both sides should agree on the title after conflict resolution";

    LOG(INFO) << "Conflict resolved to: " << GetJobTitle(cloud_after, shared_job);
}

/// Test: Deletion propagation - delete on one side, verify tombstone on other
TEST_F(SchedulerSyncBridgeE2ETest, DeletionPropagation_TombstoneConverges) {
    StartCloudSyncBridge();
    StartVehicleSyncBridge();
    EXPECT_TRUE(WaitFor([&]() { return vehicle_sync_bridge_->IsInitialized(); }, 10s));

    // Create jobs
    std::string keep_job = CreateCloudJob("Job to Keep");
    std::string delete_job = CreateCloudJob("Job to Delete");
    ASSERT_FALSE(keep_job.empty());
    ASSERT_FALSE(delete_job.empty());
    EXPECT_TRUE(WaitForConvergence(15s, "[BeforeDelete]"));

    // Delete one job on cloud
    LOG(INFO) << "=== Deleting job on cloud ===";
    EXPECT_TRUE(DeleteCloudJob(delete_job));

    // Wait for deletion to propagate (tombstone)
    EXPECT_TRUE(WaitForConvergence(20s, "[AfterDelete]"))
        << "Deletion should propagate as tombstone";

    // Verify: keep_job alive, delete_job is tombstone on both
    auto cloud_jobs = GetAllCloudJobs();
    auto vehicle_jobs = GetAllVehicleJobs();

    EXPECT_FALSE(IsJobDeleted(cloud_jobs, keep_job));
    EXPECT_FALSE(IsJobDeleted(vehicle_jobs, keep_job));
    EXPECT_TRUE(IsJobDeleted(cloud_jobs, delete_job)) << "Cloud should have tombstone";
    EXPECT_TRUE(IsJobDeleted(vehicle_jobs, delete_job)) << "Vehicle should have tombstone";
}

/// Test: Delete on one side while modifying on the other (offline)
TEST_F(SchedulerSyncBridgeE2ETest, OfflineDeleteVsModify_Convergence) {
    StartCloudSyncBridge();
    StartVehicleSyncBridge();
    EXPECT_TRUE(WaitFor([&]() { return vehicle_sync_bridge_->IsInitialized(); }, 10s));

    std::string job = CreateCloudJob("Contested Job");
    ASSERT_FALSE(job.empty());
    EXPECT_TRUE(WaitForConvergence(15s, "[Initial]"));

    LOG(INFO) << "=== Going offline ===";
    StopVehicleSyncBridge();
    StopCloudSyncBridge();
    std::this_thread::sleep_for(500ms);

    // Cloud DELETES the job, vehicle MODIFIES it
    EXPECT_TRUE(DeleteCloudJob(job));
    EXPECT_TRUE(UpdateVehicleJob(job, "Modified After Cloud Delete"));

    LOG(INFO) << "=== Reconnecting - delete vs modify conflict ===";
    StartCloudSyncBridge();
    StartVehicleSyncBridge();

    // Wait for convergence - protocol should resolve this
    EXPECT_TRUE(WaitForConvergence(25s, "[AfterDeleteVsModify]"))
        << "Delete vs modify should resolve consistently";

    // Both sides should agree (either deleted or not)
    auto cloud_jobs = GetAllCloudJobs();
    auto vehicle_jobs = GetAllVehicleJobs();
    EXPECT_EQ(IsJobDeleted(cloud_jobs, job), IsJobDeleted(vehicle_jobs, job))
        << "Both sides should agree on deletion state";

    LOG(INFO) << "Delete vs Modify resolved: deleted=" << IsJobDeleted(cloud_jobs, job);
}

/// Test: Multiple reconnect cycles maintain state consistency
TEST_F(SchedulerSyncBridgeE2ETest, MultipleReconnectCycles_StateRemainsconsistent) {
    std::vector<std::string> all_jobs;

    for (int cycle = 0; cycle < 3; ++cycle) {
        LOG(INFO) << "=== Cycle " << (cycle + 1) << " ===";

        StartCloudSyncBridge();
        StartVehicleSyncBridge();
        EXPECT_TRUE(WaitFor([&]() { return vehicle_sync_bridge_->IsInitialized(); }, 10s));

        // If not first cycle, verify previous jobs still exist
        if (cycle > 0) {
            EXPECT_TRUE(WaitForConvergence(15s, "[Cycle" + std::to_string(cycle + 1) + " Reconnect]"));
        }

        // Add new jobs this cycle
        std::string cloud_job = CreateCloudJob("Cycle " + std::to_string(cycle + 1) + " Cloud");
        std::string vehicle_job = CreateVehicleJob("Cycle " + std::to_string(cycle + 1) + " Vehicle");
        all_jobs.push_back(cloud_job);
        all_jobs.push_back(vehicle_job);

        EXPECT_TRUE(WaitForConvergence(20s, "[Cycle" + std::to_string(cycle + 1) + " Sync]"));

        StopVehicleSyncBridge();
        StopCloudSyncBridge();
        std::this_thread::sleep_for(500ms);
    }

    // Final reconnect - all 6 jobs should exist and match
    LOG(INFO) << "=== Final verification ===";
    StartCloudSyncBridge();
    StartVehicleSyncBridge();

    EXPECT_TRUE(WaitForConvergence(20s, "[Final]"))
        << "All jobs from all cycles should converge";

    EXPECT_EQ(GetAllCloudJobs().size(), 6u);
    EXPECT_EQ(GetAllVehicleJobs().size(), 6u);
}

/// Test: Large batch sync - many jobs created at once
TEST_F(SchedulerSyncBridgeE2ETest, LargeBatchSync_AllJobsConverge) {
    StartCloudSyncBridge();
    StartVehicleSyncBridge();
    EXPECT_TRUE(WaitFor([&]() { return vehicle_sync_bridge_->IsInitialized(); }, 10s));

    constexpr int BATCH_SIZE = 10;

    // Create many jobs on cloud
    for (int i = 0; i < BATCH_SIZE; ++i) {
        std::string job = CreateCloudJob("Batch Job " + std::to_string(i));
        ASSERT_FALSE(job.empty()) << "Failed to create batch job " << i;
    }

    // Wait for all to converge
    EXPECT_TRUE(WaitForConvergence(45s, "[LargeBatch]"))
        << "All " << BATCH_SIZE << " jobs should converge";

    auto cloud_jobs = GetAllCloudJobs();
    auto vehicle_jobs = GetAllVehicleJobs();
    EXPECT_EQ(cloud_jobs.size(), static_cast<size_t>(BATCH_SIZE));
    EXPECT_EQ(vehicle_jobs.size(), static_cast<size_t>(BATCH_SIZE));
}

/// Test: Complex scenario - simultaneous deletes, updates, and creates on both sides
/// This tests the full sync protocol under realistic conditions where both cloud and
/// vehicle are making changes independently.
///
/// Scenario:
/// 1. Create 8 initial jobs (4 cloud, 4 vehicle) and sync
/// 2. Go offline
/// 3. On cloud: delete 2 jobs, modify 1, create 3 new
/// 4. On vehicle: delete 2 different jobs, modify 1, create 3 new
/// 5. Reconnect and verify convergence
///
/// Expected final state:
/// - 4 original jobs deleted (tombstones)
/// - 4 original jobs remaining (2 modified)
/// - 6 new jobs (3 from each side)
/// Total: 10 live jobs + 4 tombstones = 14 jobs in final state
TEST_F(SchedulerSyncBridgeE2ETest, ComplexMixedOperations_ConvergesAfterReconnect) {
    StartCloudSyncBridge();
    StartVehicleSyncBridge();
    EXPECT_TRUE(WaitFor([&]() { return vehicle_sync_bridge_->IsInitialized(); }, 10s));

    // =========================================================================
    // Phase 1: Create initial jobs and sync
    // =========================================================================
    LOG(INFO) << "=== Phase 1: Creating initial jobs ===";

    // Create 4 jobs on cloud
    std::string cloud_job_1 = CreateCloudJob("Cloud Initial 1");  // will be deleted by cloud
    std::string cloud_job_2 = CreateCloudJob("Cloud Initial 2");  // will be deleted by cloud
    std::string cloud_job_3 = CreateCloudJob("Cloud Initial 3");  // will be modified by cloud
    std::string cloud_job_4 = CreateCloudJob("Cloud Initial 4");  // will survive unchanged

    // Create 4 jobs on vehicle
    std::string vehicle_job_1 = CreateVehicleJob("Vehicle Initial 1");  // will be deleted by vehicle
    std::string vehicle_job_2 = CreateVehicleJob("Vehicle Initial 2");  // will be deleted by vehicle
    std::string vehicle_job_3 = CreateVehicleJob("Vehicle Initial 3");  // will be modified by vehicle
    std::string vehicle_job_4 = CreateVehicleJob("Vehicle Initial 4");  // will survive unchanged

    ASSERT_FALSE(cloud_job_1.empty());
    ASSERT_FALSE(cloud_job_2.empty());
    ASSERT_FALSE(cloud_job_3.empty());
    ASSERT_FALSE(cloud_job_4.empty());
    ASSERT_FALSE(vehicle_job_1.empty());
    ASSERT_FALSE(vehicle_job_2.empty());
    ASSERT_FALSE(vehicle_job_3.empty());
    ASSERT_FALSE(vehicle_job_4.empty());

    // Wait for initial sync to complete
    EXPECT_TRUE(WaitForConvergence(30s, "[Initial 8 jobs]"))
        << "Initial 8 jobs should sync before going offline";

    auto initial_cloud = GetAllCloudJobs();
    auto initial_vehicle = GetAllVehicleJobs();
    EXPECT_EQ(initial_cloud.size(), 8u) << "Cloud should have 8 jobs";
    EXPECT_EQ(initial_vehicle.size(), 8u) << "Vehicle should have 8 jobs";

    // =========================================================================
    // Phase 2: Go offline and make changes on both sides
    // =========================================================================
    LOG(INFO) << "=== Phase 2: Going offline ===";
    StopVehicleSyncBridge();
    StopCloudSyncBridge();
    std::this_thread::sleep_for(500ms);

    // --- Cloud side changes ---
    LOG(INFO) << "Cloud: Deleting 2 jobs, modifying 1, creating 3 new";

    // Cloud deletes 2 jobs
    EXPECT_TRUE(DeleteCloudJob(cloud_job_1)) << "Cloud delete job 1";
    EXPECT_TRUE(DeleteCloudJob(cloud_job_2)) << "Cloud delete job 2";

    // Cloud modifies 1 job
    EXPECT_TRUE(UpdateCloudJob(cloud_job_3, "Cloud Initial 3 - CLOUD MODIFIED"));

    // Cloud creates 3 new jobs
    std::string cloud_new_1 = CreateCloudJob("Cloud New Offline 1");
    std::string cloud_new_2 = CreateCloudJob("Cloud New Offline 2");
    std::string cloud_new_3 = CreateCloudJob("Cloud New Offline 3");
    ASSERT_FALSE(cloud_new_1.empty());
    ASSERT_FALSE(cloud_new_2.empty());
    ASSERT_FALSE(cloud_new_3.empty());

    // --- Vehicle side changes ---
    LOG(INFO) << "Vehicle: Deleting 2 jobs, modifying 1, creating 3 new";

    // Vehicle deletes 2 DIFFERENT jobs (the ones created by vehicle)
    EXPECT_TRUE(DeleteVehicleJob(vehicle_job_1)) << "Vehicle delete job 1";
    EXPECT_TRUE(DeleteVehicleJob(vehicle_job_2)) << "Vehicle delete job 2";

    // Vehicle modifies 1 job
    EXPECT_TRUE(UpdateVehicleJob(vehicle_job_3, "Vehicle Initial 3 - VEHICLE MODIFIED"));

    // Vehicle creates 3 new jobs
    std::string vehicle_new_1 = CreateVehicleJob("Vehicle New Offline 1");
    std::string vehicle_new_2 = CreateVehicleJob("Vehicle New Offline 2");
    std::string vehicle_new_3 = CreateVehicleJob("Vehicle New Offline 3");
    ASSERT_FALSE(vehicle_new_1.empty());
    ASSERT_FALSE(vehicle_new_2.empty());
    ASSERT_FALSE(vehicle_new_3.empty());

    // Sanity check: verify sides haven't seen each other's changes yet
    auto cloud_offline = GetAllCloudJobs();
    auto vehicle_offline = GetAllVehicleJobs();
    LOG(INFO) << "After offline changes: cloud has " << cloud_offline.size()
              << " jobs, vehicle has " << vehicle_offline.size() << " jobs";

    // Cloud should see: 2 deleted (tombstones), 6 live (4 original - 2 deleted + 3 new)
    // But it doesn't see vehicle's changes yet
    size_t cloud_live = std::count_if(cloud_offline.begin(), cloud_offline.end(),
                                       [](const sched::Job& j) { return !j.deleted; });
    // Vehicle should see its own state
    size_t vehicle_live = std::count_if(vehicle_offline.begin(), vehicle_offline.end(),
                                         [](const sched::Job& j) { return !j.deleted; });

    LOG(INFO) << "Cloud offline: " << cloud_live << " live, " << (cloud_offline.size() - cloud_live) << " deleted";
    LOG(INFO) << "Vehicle offline: " << vehicle_live << " live, " << (vehicle_offline.size() - vehicle_live) << " deleted";

    // =========================================================================
    // Phase 3: Reconnect and verify convergence
    // =========================================================================
    LOG(INFO) << "=== Phase 3: Reconnecting ===";
    StartCloudSyncBridge();
    StartVehicleSyncBridge();

    // This is the key test: complex bidirectional sync should converge
    EXPECT_TRUE(WaitForConvergence(45s, "[Complex Mixed Operations]"))
        << "Complex bidirectional changes should converge";

    // =========================================================================
    // Phase 4: Verify final state
    // =========================================================================
    LOG(INFO) << "=== Phase 4: Verifying final state ===";

    auto final_cloud = GetAllCloudJobs();
    auto final_vehicle = GetAllVehicleJobs();

    // Count totals
    size_t final_cloud_total = final_cloud.size();
    size_t final_vehicle_total = final_vehicle.size();
    size_t final_cloud_deleted = std::count_if(final_cloud.begin(), final_cloud.end(),
                                                [](const sched::Job& j) { return j.deleted; });
    size_t final_vehicle_deleted = std::count_if(final_vehicle.begin(), final_vehicle.end(),
                                                  [](const sched::Job& j) { return j.deleted; });

    LOG(INFO) << "Final cloud: " << final_cloud_total << " total, "
              << final_cloud_deleted << " deleted, "
              << (final_cloud_total - final_cloud_deleted) << " live";
    LOG(INFO) << "Final vehicle: " << final_vehicle_total << " total, "
              << final_vehicle_deleted << " deleted, "
              << (final_vehicle_total - final_vehicle_deleted) << " live";

    // Verify counts match between sides
    EXPECT_EQ(final_cloud_total, final_vehicle_total) << "Total job count should match";
    EXPECT_EQ(final_cloud_deleted, final_vehicle_deleted) << "Deleted count should match";

    // Expected: 4 deleted jobs (2 from cloud + 2 from vehicle)
    EXPECT_EQ(final_cloud_deleted, 4u) << "Should have 4 tombstones";

    // Expected live jobs: 4 original survivors + 6 new = 10
    size_t expected_live = 10u;
    EXPECT_EQ(final_cloud_total - final_cloud_deleted, expected_live) << "Should have 10 live jobs";

    // Verify specific deletions propagated
    EXPECT_TRUE(IsJobDeleted(final_cloud, cloud_job_1)) << "cloud_job_1 should be deleted";
    EXPECT_TRUE(IsJobDeleted(final_cloud, cloud_job_2)) << "cloud_job_2 should be deleted";
    EXPECT_TRUE(IsJobDeleted(final_vehicle, vehicle_job_1)) << "vehicle_job_1 should be deleted";
    EXPECT_TRUE(IsJobDeleted(final_vehicle, vehicle_job_2)) << "vehicle_job_2 should be deleted";

    // Verify survivors are not deleted
    EXPECT_FALSE(IsJobDeleted(final_cloud, cloud_job_4)) << "cloud_job_4 should survive";
    EXPECT_FALSE(IsJobDeleted(final_vehicle, vehicle_job_4)) << "vehicle_job_4 should survive";

    // Verify new jobs exist on both sides
    EXPECT_TRUE(FindJobById(final_cloud, cloud_new_1) != nullptr) << "cloud_new_1 on cloud";
    EXPECT_TRUE(FindJobById(final_cloud, cloud_new_2) != nullptr) << "cloud_new_2 on cloud";
    EXPECT_TRUE(FindJobById(final_cloud, cloud_new_3) != nullptr) << "cloud_new_3 on cloud";
    EXPECT_TRUE(FindJobById(final_vehicle, cloud_new_1) != nullptr) << "cloud_new_1 on vehicle";
    EXPECT_TRUE(FindJobById(final_vehicle, cloud_new_2) != nullptr) << "cloud_new_2 on vehicle";
    EXPECT_TRUE(FindJobById(final_vehicle, cloud_new_3) != nullptr) << "cloud_new_3 on vehicle";

    EXPECT_TRUE(FindJobById(final_cloud, vehicle_new_1) != nullptr) << "vehicle_new_1 on cloud";
    EXPECT_TRUE(FindJobById(final_cloud, vehicle_new_2) != nullptr) << "vehicle_new_2 on cloud";
    EXPECT_TRUE(FindJobById(final_cloud, vehicle_new_3) != nullptr) << "vehicle_new_3 on cloud";
    EXPECT_TRUE(FindJobById(final_vehicle, vehicle_new_1) != nullptr) << "vehicle_new_1 on vehicle";
    EXPECT_TRUE(FindJobById(final_vehicle, vehicle_new_2) != nullptr) << "vehicle_new_2 on vehicle";
    EXPECT_TRUE(FindJobById(final_vehicle, vehicle_new_3) != nullptr) << "vehicle_new_3 on vehicle";

    // Verify modified jobs have consistent titles (may be either version depending on authority)
    std::string cloud_3_title_on_cloud = GetJobTitle(final_cloud, cloud_job_3);
    std::string cloud_3_title_on_vehicle = GetJobTitle(final_vehicle, cloud_job_3);
    EXPECT_EQ(cloud_3_title_on_cloud, cloud_3_title_on_vehicle)
        << "cloud_job_3 should have same title on both sides";

    std::string vehicle_3_title_on_cloud = GetJobTitle(final_cloud, vehicle_job_3);
    std::string vehicle_3_title_on_vehicle = GetJobTitle(final_vehicle, vehicle_job_3);
    EXPECT_EQ(vehicle_3_title_on_cloud, vehicle_3_title_on_vehicle)
        << "vehicle_job_3 should have same title on both sides";

    LOG(INFO) << "cloud_job_3 final title: " << cloud_3_title_on_cloud;
    LOG(INFO) << "vehicle_job_3 final title: " << vehicle_3_title_on_cloud;

    // =========================================================================
    // Phase 5: Pause and delete operations while sync is established
    // =========================================================================
    LOG(INFO) << "=== Phase 5: Post-convergence pause and delete ===";

    // Pause cloud_job_4 from cloud side
    LOG(INFO) << "Pausing cloud_job_4 from cloud side";
    ASSERT_TRUE(PauseCloudJob(cloud_job_4)) << "Should be able to pause cloud_job_4";

    // Delete vehicle_job_4 from vehicle side
    LOG(INFO) << "Deleting vehicle_job_4 from vehicle side";
    ASSERT_TRUE(DeleteVehicleJob(vehicle_job_4)) << "Should be able to delete vehicle_job_4";

    // Wait for convergence again
    EXPECT_TRUE(WaitForConvergence(20s, "[Post-convergence pause/delete]"))
        << "Pause and delete should sync to establish consistent state";

    // =========================================================================
    // Phase 6: Verify pause and delete propagated correctly
    // =========================================================================
    LOG(INFO) << "=== Phase 6: Verifying pause and delete propagated ===";

    auto phase5_cloud = GetAllCloudJobs();
    auto phase5_vehicle = GetAllVehicleJobs();

    // Count state changes
    size_t phase5_cloud_deleted = std::count_if(phase5_cloud.begin(), phase5_cloud.end(),
                                                 [](const sched::Job& j) { return j.deleted; });
    size_t phase5_vehicle_deleted = std::count_if(phase5_vehicle.begin(), phase5_vehicle.end(),
                                                   [](const sched::Job& j) { return j.deleted; });
    size_t phase5_cloud_paused = std::count_if(phase5_cloud.begin(), phase5_cloud.end(),
                                                [](const sched::Job& j) { return j.paused && !j.deleted; });
    size_t phase5_vehicle_paused = std::count_if(phase5_vehicle.begin(), phase5_vehicle.end(),
                                                  [](const sched::Job& j) { return j.paused && !j.deleted; });

    LOG(INFO) << "Phase 5 cloud: " << phase5_cloud_deleted << " deleted, " << phase5_cloud_paused << " paused";
    LOG(INFO) << "Phase 5 vehicle: " << phase5_vehicle_deleted << " deleted, " << phase5_vehicle_paused << " paused";

    // Verify counts match
    EXPECT_EQ(phase5_cloud_deleted, phase5_vehicle_deleted)
        << "Deleted count should match between cloud and vehicle";
    EXPECT_EQ(phase5_cloud_paused, phase5_vehicle_paused)
        << "Paused count should match between cloud and vehicle";

    // Expected: 5 deleted (4 from earlier + vehicle_job_4)
    EXPECT_EQ(phase5_cloud_deleted, 5u) << "Should now have 5 tombstones";

    // Verify specific operations propagated
    // cloud_job_4 should be paused (not deleted) on both sides
    EXPECT_TRUE(IsJobPaused(phase5_cloud, cloud_job_4))
        << "cloud_job_4 should be paused on cloud";
    EXPECT_TRUE(IsJobPaused(phase5_vehicle, cloud_job_4))
        << "cloud_job_4 should be paused on vehicle";
    EXPECT_FALSE(IsJobDeleted(phase5_cloud, cloud_job_4))
        << "cloud_job_4 should NOT be deleted on cloud";
    EXPECT_FALSE(IsJobDeleted(phase5_vehicle, cloud_job_4))
        << "cloud_job_4 should NOT be deleted on vehicle";

    // vehicle_job_4 should be deleted on both sides
    EXPECT_TRUE(IsJobDeleted(phase5_cloud, vehicle_job_4))
        << "vehicle_job_4 should be deleted on cloud";
    EXPECT_TRUE(IsJobDeleted(phase5_vehicle, vehicle_job_4))
        << "vehicle_job_4 should be deleted on vehicle";

    LOG(INFO) << "=== Complex mixed operations test PASSED ===";
}

/// Test: Health endpoints work correctly
TEST_F(SchedulerSyncBridgeE2ETest, HealthEndpoints_ReportCorrectly) {
    StartCloudSyncBridge();
    StartVehicleSyncBridge();

    EXPECT_TRUE(vehicle_sync_bridge_->IsRunning());
    EXPECT_TRUE(WaitFor([&]() { return vehicle_sync_bridge_->IsInitialized(); }, 10s));

    // Check cloud sync bridge health
    auto channel = grpc::CreateChannel(
        "localhost:" + std::to_string(cloud_sync_bridge_port_),
        grpc::InsecureChannelCredentials());
    auto stub = swdv::cloud_scheduler_sync_bridge::healthy_service::NewStub(channel);

    grpc::ClientContext context;
    swdv::cloud_scheduler_sync_bridge::healthy_request request;
    swdv::cloud_scheduler_sync_bridge::healthy_response response;

    auto status = stub->healthy(&context, request, &response);
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(response.is_healthy());
}

}  // namespace ifex::test

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    ::testing::InitGoogleTest(&argc, argv);
    mosquitto_lib_init();

    // Use GlobalEnvironment to start/stop services once for all tests
    class Environment : public ::testing::Environment {
    public:
        void SetUp() override { IntegrationTestFixture::GlobalSetUp(); }
        void TearDown() override { IntegrationTestFixture::GlobalTearDown(); }
    };
    ::testing::AddGlobalTestEnvironment(new Environment);

    int result = RUN_ALL_TESTS();
    mosquitto_lib_cleanup();
    return result;
}
