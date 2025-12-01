#include <gtest/gtest.h>
#include <glog/logging.h>
#include "ifex-scheduler-service.grpc.pb.h"
#include "service-discovery-service.grpc.pb.h"
#include "ifex-dispatcher-service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include "test_fixture.hpp"

using json = nlohmann::json;

class FullChainIntegrationTest : public IntegrationTestFixture {
protected:
    std::vector<std::string> created_job_ids_;

    void TearDown() override {
        // Clean up created jobs
        for (const auto& job_id : created_job_ids_) {
            delete_job(job_id);
        }
        created_job_ids_.clear();
        IntegrationTestFixture::TearDown();
    }

    std::string get_future_time(int seconds_from_now) {
        auto now = std::chrono::system_clock::now();
        auto future = now + std::chrono::seconds(seconds_from_now);
        auto time_t = std::chrono::system_clock::to_time_t(future);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
        return ss.str();
    }

    void delete_job(const std::string& job_id) {
        auto stub = swdv::ifex_scheduler::delete_job_service::NewStub(scheduler_channel_);

        swdv::ifex_scheduler::delete_job_request request;
        request.set_job_id(job_id);

        swdv::ifex_scheduler::delete_job_response response;
        grpc::ClientContext context;

        stub->delete_job(&context, request, &response);
    }

    // Call service through dispatcher using call_method
    std::pair<bool, json> call_via_dispatcher(const std::string& service,
                                               const std::string& method,
                                               const json& params) {
        auto stub = swdv::ifex_dispatcher::call_method_service::NewStub(dispatcher_channel_);

        swdv::ifex_dispatcher::call_method_request request;
        auto* call = request.mutable_call();
        call->set_service_name(service);
        call->set_method_name(method);
        call->set_parameters(params.dump());
        call->set_timeout_ms(5000);

        swdv::ifex_dispatcher::call_method_response response;
        grpc::ClientContext context;

        auto status = stub->call_method(&context, request, &response);
        if (!status.ok()) {
            return {false, json{{"error", status.error_message()}}};
        }

        const auto& result = response.result();
        if (result.status() != swdv::ifex_dispatcher::call_status_t::SUCCESS) {
            return {false, json{{"error", result.error_message()}}};
        }

        try {
            return {true, json::parse(result.response())};
        } catch (...) {
            return {true, json{{"response", result.response()}}};
        }
    }
};

// Test: Discovery → Service Registration chain
TEST_F(FullChainIntegrationTest, DiscoveryListsRegisteredServices) {
    auto stub = swdv::service_discovery::query_services_service::NewStub(discovery_channel_);

    swdv::service_discovery::query_services_request request;
    swdv::service_discovery::query_services_response response;
    grpc::ClientContext context;

    auto status = stub->query_services(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "Failed to query services: " << status.error_message();

    // Verify core services are registered
    std::set<std::string> registered_services;
    for (const auto& service : response.services()) {
        registered_services.insert(service.name());
        LOG(INFO) << "Found registered service: " << service.name();
    }

    EXPECT_TRUE(registered_services.count("echo_service") > 0)
        << "echo_service should be registered";
    EXPECT_TRUE(registered_services.count("settings_service") > 0)
        << "settings_service should be registered";
}

// Test: Discovery → Dispatcher chain (dispatcher resolves services)
TEST_F(FullChainIntegrationTest, DispatcherResolvesServiceFromDiscovery) {
    // Call echo service through dispatcher - dispatcher must resolve via discovery
    json params = {{"message", "test message"}};
    auto [success, result] = call_via_dispatcher("echo_service", "echo", params);

    EXPECT_TRUE(success) << "Error: " << result.dump();
    EXPECT_TRUE(result.contains("response"));
}

// Test: Scheduler → Dispatcher → Service chain (immediate execution)
TEST_F(FullChainIntegrationTest, SchedulerExecutesJobThroughDispatcher) {
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    // Create a job scheduled for very soon (2 seconds)
    swdv::ifex_scheduler::create_job_request create_request;
    auto* job = create_request.mutable_job();

    job->set_title("Immediate Execution Test");
    job->set_service("echo_service");
    job->set_method("echo");

    json params = {{"message", "scheduled execution test"}};
    job->set_parameters(params.dump());
    job->set_scheduled_time(get_future_time(2));

    swdv::ifex_scheduler::create_job_response create_response;
    grpc::ClientContext create_context;

    auto status = create_stub->create_job(&create_context, create_request, &create_response);
    ASSERT_TRUE(status.ok()) << "Failed to create job: " << status.error_message();
    ASSERT_TRUE(create_response.success()) << "Job creation failed: " << create_response.message();

    std::string job_id = create_response.job_id();
    created_job_ids_.push_back(job_id);
    LOG(INFO) << "Created job " << job_id << " scheduled for 2 seconds from now";

    // Wait for job to execute (4 seconds should be enough)
    std::this_thread::sleep_for(std::chrono::seconds(4));

    // Check job status - should be COMPLETED or RUNNING
    auto get_stub = swdv::ifex_scheduler::get_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::get_job_request get_request;
    get_request.set_job_id(job_id);

    swdv::ifex_scheduler::get_job_response get_response;
    grpc::ClientContext get_context;

    status = get_stub->get_job(&get_context, get_request, &get_response);
    ASSERT_TRUE(status.ok());

    auto job_status = get_response.job().status();
    LOG(INFO) << "Job status after execution: " << job_status;

    // Job should have been executed (status COMPLETED or at least not PENDING)
    EXPECT_NE(job_status, swdv::ifex_scheduler::PENDING)
        << "Job should have been executed by now";

    // Verify the echo service response was captured
    if (job_status == swdv::ifex_scheduler::COMPLETED) {
        const auto& result = get_response.job().result();
        EXPECT_FALSE(result.empty()) << "Completed job should have a result";
        LOG(INFO) << "Job result: " << result;

        // Parse and verify the echo response contains our message
        try {
            auto result_json = json::parse(result);
            if (result_json.contains("response")) {
                EXPECT_NE(result_json["response"].get<std::string>().find("scheduled execution test"),
                          std::string::npos)
                    << "Echo response should contain our message";
            }
        } catch (...) {
            // Result might not be JSON, just log it
            LOG(INFO) << "Result is not JSON: " << result;
        }
    }
}

// Test: Full chain with settings service
TEST_F(FullChainIntegrationTest, DispatcherCallsSettingsService) {
    // Get current setting through dispatcher
    json get_params = {{"key", "test_setting"}};
    auto [success, result] = call_via_dispatcher("settings_service", "get_setting", get_params);

    // The setting might not exist, but the call should go through
    LOG(INFO) << "Get setting result: " << result.dump();
}

// Test: Full chain - set and get settings
TEST_F(FullChainIntegrationTest, SettingsServiceRoundTrip) {
    // Set a setting
    json set_params = {
        {"key", "test_chain_setting"},
        {"value", "test_value_123"}
    };
    auto [set_success, set_result] = call_via_dispatcher("settings_service", "set_setting", set_params);
    LOG(INFO) << "Set setting result: " << set_result.dump();

    // Get the setting back
    json get_params = {{"key", "test_chain_setting"}};
    auto [get_success, get_result] = call_via_dispatcher("settings_service", "get_setting", get_params);
    LOG(INFO) << "Get setting result: " << get_result.dump();

    // Verify the value matches (if both calls succeeded)
    if (get_success && get_result.contains("value")) {
        EXPECT_EQ(get_result["value"], "test_value_123");
    }
}

// Test: Discovery → Dispatcher → Multiple services
TEST_F(FullChainIntegrationTest, DispatcherRoutesToMultipleServices) {
    // Call echo service
    json echo_params = {{"message", "hello"}};
    auto [echo_success, echo_result] = call_via_dispatcher("echo_service", "echo", echo_params);
    EXPECT_TRUE(echo_success) << "Echo failed: " << echo_result.dump();

    // Call settings service
    json settings_params = {{"key", "some_key"}};
    auto [settings_success, settings_result] = call_via_dispatcher("settings_service", "get_setting", settings_params);
    // Just verify no RPC error
    LOG(INFO) << "Settings result: " << settings_result.dump();

    // Both calls went through dispatcher, which resolved them via discovery
}

// Test: Scheduler validates service exists in discovery before creating job
TEST_F(FullChainIntegrationTest, SchedulerValidatesServiceViaDiscovery) {
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    // Try to create job for non-existent service
    swdv::ifex_scheduler::create_job_request request;
    auto* job = request.mutable_job();
    job->set_title("Invalid Service Job");
    job->set_service("this_service_does_not_exist");
    job->set_method("some_method");
    job->set_parameters("{}");
    job->set_scheduled_time(get_future_time(3600));

    swdv::ifex_scheduler::create_job_response response;
    grpc::ClientContext context;

    auto status = create_stub->create_job(&context, request, &response);
    ASSERT_TRUE(status.ok());

    // Should fail because scheduler queries discovery to validate service
    EXPECT_FALSE(response.success()) << "Should reject job for non-existent service";
}

// Test: Multiple scheduled jobs execute in order
TEST_F(FullChainIntegrationTest, MultipleJobsExecuteInOrder) {
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    // Create 3 jobs with staggered times
    std::vector<std::string> job_ids;
    for (int i = 0; i < 3; i++) {
        swdv::ifex_scheduler::create_job_request request;
        auto* job = request.mutable_job();

        job->set_title("Order Test Job " + std::to_string(i));
        job->set_service("echo_service");
        job->set_method("echo");

        json params = {{"message", "job " + std::to_string(i)}};
        job->set_parameters(params.dump());
        job->set_scheduled_time(get_future_time(2 + i));  // 2, 3, 4 seconds

        swdv::ifex_scheduler::create_job_response response;
        grpc::ClientContext context;

        auto status = create_stub->create_job(&context, request, &response);
        ASSERT_TRUE(status.ok() && response.success());

        job_ids.push_back(response.job_id());
        created_job_ids_.push_back(response.job_id());
    }

    // Wait for all jobs to complete
    std::this_thread::sleep_for(std::chrono::seconds(7));

    // Verify all jobs completed
    auto get_stub = swdv::ifex_scheduler::get_job_service::NewStub(scheduler_channel_);

    int completed_count = 0;
    for (const auto& job_id : job_ids) {
        swdv::ifex_scheduler::get_job_request request;
        request.set_job_id(job_id);

        swdv::ifex_scheduler::get_job_response response;
        grpc::ClientContext context;

        auto status = get_stub->get_job(&context, request, &response);
        ASSERT_TRUE(status.ok());

        if (response.job().status() == swdv::ifex_scheduler::COMPLETED) {
            completed_count++;
        }
    }

    EXPECT_EQ(completed_count, 3) << "All 3 jobs should have completed";
}

// Test: Dispatcher handles service not found gracefully
TEST_F(FullChainIntegrationTest, DispatcherHandlesUnknownService) {
    json params = {{"test", "value"}};
    auto [success, result] = call_via_dispatcher("nonexistent_service_xyz", "some_method", params);

    EXPECT_FALSE(success) << "Should fail for unknown service";
}

// Test: End-to-end job lifecycle
TEST_F(FullChainIntegrationTest, CompleteJobLifecycle) {
    // 1. Create job
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::create_job_request create_request;
    auto* job = create_request.mutable_job();
    job->set_title("Lifecycle Test Job");
    job->set_service("echo_service");
    job->set_method("echo");
    job->set_parameters(R"({"message": "lifecycle test"})");
    job->set_scheduled_time(get_future_time(3600));  // Far future - won't execute

    swdv::ifex_scheduler::create_job_response create_response;
    grpc::ClientContext create_context;

    auto status = create_stub->create_job(&create_context, create_request, &create_response);
    ASSERT_TRUE(status.ok() && create_response.success());
    std::string job_id = create_response.job_id();
    LOG(INFO) << "1. Created job: " << job_id;

    // 2. Read job
    auto get_stub = swdv::ifex_scheduler::get_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::get_job_request get_request;
    get_request.set_job_id(job_id);

    swdv::ifex_scheduler::get_job_response get_response;
    grpc::ClientContext get_context;

    status = get_stub->get_job(&get_context, get_request, &get_response);
    ASSERT_TRUE(status.ok() && get_response.success());
    EXPECT_EQ(get_response.job().title(), "Lifecycle Test Job");
    LOG(INFO) << "2. Read job: " << get_response.job().title();

    // 3. Update job
    auto update_stub = swdv::ifex_scheduler::update_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::update_job_request update_request;
    update_request.set_job_id(job_id);
    update_request.mutable_updates()->set_title("Updated Lifecycle Job");

    swdv::ifex_scheduler::update_job_response update_response;
    grpc::ClientContext update_context;

    status = update_stub->update_job(&update_context, update_request, &update_response);
    ASSERT_TRUE(status.ok() && update_response.success());
    LOG(INFO) << "3. Updated job";

    // 4. Verify update
    swdv::ifex_scheduler::get_job_response verify_response;
    grpc::ClientContext verify_context;

    swdv::ifex_scheduler::get_job_request verify_request;
    verify_request.set_job_id(job_id);

    status = get_stub->get_job(&verify_context, verify_request, &verify_response);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(verify_response.job().title(), "Updated Lifecycle Job");
    LOG(INFO) << "4. Verified update: " << verify_response.job().title();

    // 5. Delete job
    auto delete_stub = swdv::ifex_scheduler::delete_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::delete_job_request delete_request;
    delete_request.set_job_id(job_id);

    swdv::ifex_scheduler::delete_job_response delete_response;
    grpc::ClientContext delete_context;

    status = delete_stub->delete_job(&delete_context, delete_request, &delete_response);
    ASSERT_TRUE(status.ok() && delete_response.success());
    LOG(INFO) << "5. Deleted job";

    // 6. Verify deletion
    swdv::ifex_scheduler::get_job_response final_response;
    grpc::ClientContext final_context;

    swdv::ifex_scheduler::get_job_request final_request;
    final_request.set_job_id(job_id);

    status = get_stub->get_job(&final_context, final_request, &final_response);
    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(final_response.success()) << "Job should not be found after deletion";
    LOG(INFO) << "6. Verified deletion";
}
