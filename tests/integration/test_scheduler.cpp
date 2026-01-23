#include <gtest/gtest.h>
#include <glog/logging.h>
#include "scheduler-service.grpc.pb.h"
#include "scheduler-types.pb.h"
#include "discovery-service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include "test_fixture.hpp"

using json = nlohmann::json;

class SchedulerIntegrationTest : public IntegrationTestFixture {
protected:
    std::vector<std::string> created_job_ids_;

    void SetUp() override {
        IntegrationTestFixture::SetUp();
        // Wait for echo_service to be discoverable
        // This is needed because the scheduler validates services against discovery
        WaitForEchoService();
    }

    void WaitForEchoService() {
        auto stub = swdv::service_discovery::get_service_service::NewStub(discovery_channel_);

        for (int i = 0; i < 50; i++) {  // Try for 5 seconds max
            swdv::service_discovery::get_service_request request;
            request.set_service_name("echo_service");

            swdv::service_discovery::get_service_response response;
            grpc::ClientContext context;

            auto status = stub->get_service(&context, request, &response);
            if (status.ok() && !response.service_info().name().empty()) {
                LOG(INFO) << "echo_service is ready in discovery";
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        LOG(WARNING) << "echo_service not found in discovery after timeout";
    }

    void TearDown() override {
        // Clean up created jobs
        for (const auto& job_id : created_job_ids_) {
            delete_job(job_id);
        }
        created_job_ids_.clear();
        IntegrationTestFixture::TearDown();
    }

    // Returns epoch milliseconds for a time seconds from now
    uint64_t get_future_time_ms(int seconds_from_now) {
        auto now = std::chrono::system_clock::now();
        auto future = now + std::chrono::seconds(seconds_from_now);
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            future.time_since_epoch()).count();
    }

    uint64_t get_current_time_ms() {
        return get_future_time_ms(0);
    }

    // Keep for backward compatibility with any remaining string usages
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
};

TEST_F(SchedulerIntegrationTest, CreateAndGetJob) {
    // Create a job
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::create_job_request create_request;
    auto* job = create_request.mutable_job();

    job->set_title("Test Job");
    job->set_service("echo_service");
    job->set_method("echo");

    json params;
    params["message"] = "Scheduled test message";
    job->set_parameters(params.dump());

    // Schedule for 1 hour from now (won't execute during test)
    job->set_scheduled_time_ms(get_future_time_ms(3600));

    swdv::ifex_scheduler::create_job_response create_response;
    grpc::ClientContext create_context;

    auto status = create_stub->create_job(&create_context, create_request, &create_response);
    ASSERT_TRUE(status.ok()) << "Failed to create job: " << status.error_message();
    EXPECT_TRUE(create_response.success()) << "Job creation failed: " << create_response.message();
    EXPECT_FALSE(create_response.job_id().empty());

    std::string job_id = create_response.job_id();
    created_job_ids_.push_back(job_id);
    LOG(INFO) << "Created job with ID: " << job_id;

    // Get the job
    auto get_stub = swdv::ifex_scheduler::get_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::get_job_request get_request;
    get_request.set_job_id(job_id);

    swdv::ifex_scheduler::get_job_response get_response;
    grpc::ClientContext get_context;

    status = get_stub->get_job(&get_context, get_request, &get_response);
    ASSERT_TRUE(status.ok()) << "Failed to get job: " << status.error_message();
    EXPECT_TRUE(get_response.success());

    const auto& retrieved_job = get_response.job();
    EXPECT_EQ(retrieved_job.id(), job_id);
    EXPECT_EQ(retrieved_job.title(), "Test Job");
    EXPECT_EQ(retrieved_job.service(), "echo_service");
    EXPECT_EQ(retrieved_job.method(), "echo");
    EXPECT_EQ(retrieved_job.status(), swdv::scheduler_types::JOB_STATUS_PENDING);
}

TEST_F(SchedulerIntegrationTest, ListJobs) {
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    // Create multiple jobs
    for (int i = 0; i < 3; i++) {
        swdv::ifex_scheduler::create_job_request request;
        auto* job = request.mutable_job();

        job->set_title("List Test Job " + std::to_string(i));
        job->set_service("echo_service");
        job->set_method("echo");
        job->set_parameters(R"({"message": "test"})");
        job->set_scheduled_time_ms(get_future_time_ms(3600 + i));

        swdv::ifex_scheduler::create_job_response response;
        grpc::ClientContext context;

        auto status = create_stub->create_job(&context, request, &response);
        ASSERT_TRUE(status.ok());
        ASSERT_TRUE(response.success());
        created_job_ids_.push_back(response.job_id());
    }

    // List all jobs
    auto list_stub = swdv::ifex_scheduler::get_jobs_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::get_jobs_request list_request;
    // No filter - get all jobs

    swdv::ifex_scheduler::get_jobs_response list_response;
    grpc::ClientContext list_context;

    auto status = list_stub->get_jobs(&list_context, list_request, &list_response);
    ASSERT_TRUE(status.ok()) << "Failed to list jobs: " << status.error_message();
    EXPECT_TRUE(list_response.success());

    // Should have at least the 3 jobs we created
    EXPECT_GE(list_response.jobs_size(), 3);

    // Verify our jobs are in the list
    int found = 0;
    for (const auto& job : list_response.jobs()) {
        if (job.title().find("List Test Job") != std::string::npos) {
            found++;
        }
    }
    EXPECT_EQ(found, 3);
}

TEST_F(SchedulerIntegrationTest, UpdateJob) {
    // Create a job
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::create_job_request create_request;
    auto* job = create_request.mutable_job();
    job->set_title("Original Title");
    job->set_service("echo_service");
    job->set_method("echo");
    job->set_parameters(R"({"message": "original"})");
    job->set_scheduled_time_ms(get_future_time_ms(3600));

    swdv::ifex_scheduler::create_job_response create_response;
    grpc::ClientContext create_context;

    auto status = create_stub->create_job(&create_context, create_request, &create_response);
    ASSERT_TRUE(status.ok() && create_response.success());

    std::string job_id = create_response.job_id();
    created_job_ids_.push_back(job_id);

    // Update the job
    auto update_stub = swdv::ifex_scheduler::update_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::update_job_request update_request;
    update_request.set_job_id(job_id);
    auto* updates = update_request.mutable_updates();
    updates->set_title("Updated Title");
    updates->set_parameters(R"({"message": "updated"})");

    swdv::ifex_scheduler::update_job_response update_response;
    grpc::ClientContext update_context;

    status = update_stub->update_job(&update_context, update_request, &update_response);
    ASSERT_TRUE(status.ok()) << "Failed to update job: " << status.error_message();
    EXPECT_TRUE(update_response.success()) << "Update failed: " << update_response.message();

    // Verify the update
    auto get_stub = swdv::ifex_scheduler::get_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::get_job_request get_request;
    get_request.set_job_id(job_id);

    swdv::ifex_scheduler::get_job_response get_response;
    grpc::ClientContext get_context;

    status = get_stub->get_job(&get_context, get_request, &get_response);
    ASSERT_TRUE(status.ok());

    EXPECT_EQ(get_response.job().title(), "Updated Title");
}

TEST_F(SchedulerIntegrationTest, DeleteJob) {
    // Create a job
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::create_job_request create_request;
    auto* job = create_request.mutable_job();
    job->set_title("Job to Delete");
    job->set_service("echo_service");
    job->set_method("echo");
    job->set_parameters(R"({"message": "delete me"})");
    job->set_scheduled_time_ms(get_future_time_ms(3600));

    swdv::ifex_scheduler::create_job_response create_response;
    grpc::ClientContext create_context;

    auto status = create_stub->create_job(&create_context, create_request, &create_response);
    ASSERT_TRUE(status.ok() && create_response.success());

    std::string job_id = create_response.job_id();
    // Don't add to cleanup list since we're deleting it

    // Delete the job
    auto delete_stub = swdv::ifex_scheduler::delete_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::delete_job_request delete_request;
    delete_request.set_job_id(job_id);

    swdv::ifex_scheduler::delete_job_response delete_response;
    grpc::ClientContext delete_context;

    status = delete_stub->delete_job(&delete_context, delete_request, &delete_response);
    ASSERT_TRUE(status.ok()) << "Failed to delete job: " << status.error_message();
    EXPECT_TRUE(delete_response.success()) << "Delete failed: " << delete_response.message();

    // Verify job is gone
    auto get_stub = swdv::ifex_scheduler::get_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::get_job_request get_request;
    get_request.set_job_id(job_id);

    swdv::ifex_scheduler::get_job_response get_response;
    grpc::ClientContext get_context;

    status = get_stub->get_job(&get_context, get_request, &get_response);
    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(get_response.success()) << "Job should not be found after deletion";
}

TEST_F(SchedulerIntegrationTest, GetCalendarView) {
    // Create jobs for today
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    for (int i = 0; i < 3; i++) {
        swdv::ifex_scheduler::create_job_request request;
        auto* job = request.mutable_job();
        job->set_title("Calendar Job " + std::to_string(i));
        job->set_service("echo_service");
        job->set_method("echo");
        job->set_parameters(R"({"message": "calendar test"})");
        // Schedule within the next hour
        job->set_scheduled_time_ms(get_future_time_ms(60 * (i + 1)));

        swdv::ifex_scheduler::create_job_response response;
        grpc::ClientContext context;

        auto status = create_stub->create_job(&context, request, &response);
        ASSERT_TRUE(status.ok() && response.success());
        created_job_ids_.push_back(response.job_id());
    }

    // Get calendar view for today
    auto calendar_stub = swdv::ifex_scheduler::get_calendar_view_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::get_calendar_view_request request;
    request.set_view_type(swdv::ifex_scheduler::DAY);
    request.set_reference_time_ms(get_current_time_ms());

    swdv::ifex_scheduler::get_calendar_view_response response;
    grpc::ClientContext context;

    auto status = calendar_stub->get_calendar_view(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "Failed to get calendar view: " << status.error_message();
    EXPECT_TRUE(response.success());

    // Should have our calendar jobs
    int found = 0;
    for (const auto& job : response.jobs()) {
        if (job.title().find("Calendar Job") != std::string::npos) {
            found++;
        }
    }
    EXPECT_GE(found, 3);
}

TEST_F(SchedulerIntegrationTest, CreateJobForNonExistentService) {
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::create_job_request request;
    auto* job = request.mutable_job();
    job->set_title("Invalid Service Job");
    job->set_service("non_existent_service");
    job->set_method("some_method");
    job->set_parameters("{}");
    job->set_scheduled_time_ms(get_future_time_ms(3600));

    swdv::ifex_scheduler::create_job_response response;
    grpc::ClientContext context;

    auto status = create_stub->create_job(&context, request, &response);
    ASSERT_TRUE(status.ok());

    // Should fail because service doesn't exist
    EXPECT_FALSE(response.success()) << "Should reject job for non-existent service";
    EXPECT_FALSE(response.message().empty()) << "Should have error message";
}

TEST_F(SchedulerIntegrationTest, CreateRecurringJob) {
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::create_job_request request;
    auto* job = request.mutable_job();
    job->set_title("Recurring Job");
    job->set_service("echo_service");
    job->set_method("echo");
    job->set_parameters(R"({"message": "recurring"})");
    job->set_scheduled_time_ms(get_future_time_ms(3600));
    job->set_recurrence_rule("daily");

    swdv::ifex_scheduler::create_job_response response;
    grpc::ClientContext context;

    auto status = create_stub->create_job(&context, request, &response);
    ASSERT_TRUE(status.ok()) << "Failed to create recurring job: " << status.error_message();
    EXPECT_TRUE(response.success()) << "Recurring job creation failed: " << response.message();

    std::string job_id = response.job_id();
    created_job_ids_.push_back(job_id);

    // Verify recurrence rule is set
    auto get_stub = swdv::ifex_scheduler::get_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::get_job_request get_request;
    get_request.set_job_id(job_id);

    swdv::ifex_scheduler::get_job_response get_response;
    grpc::ClientContext get_context;

    status = get_stub->get_job(&get_context, get_request, &get_response);
    ASSERT_TRUE(status.ok());

    EXPECT_EQ(get_response.job().recurrence_rule(), "daily");
}

/**
 * @test Jobs should survive scheduler restart (persistence test)
 *
 * This test will FAIL until persistence is implemented in the scheduler.
 * Once persistence is added, jobs created before shutdown should be
 * available after restart.
 */
TEST_F(SchedulerIntegrationTest, JobsSurviveRestart) {
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    // Create several jobs
    std::vector<std::string> job_ids;
    for (int i = 0; i < 3; i++) {
        swdv::ifex_scheduler::create_job_request request;
        auto* job = request.mutable_job();

        job->set_title("Persistence Test Job " + std::to_string(i));
        job->set_service("echo_service");
        job->set_method("echo");
        job->set_parameters(R"({"message": "persist me"})");
        job->set_scheduled_time_ms(get_future_time_ms(3600));  // Far future

        swdv::ifex_scheduler::create_job_response response;
        grpc::ClientContext context;

        auto status = create_stub->create_job(&context, request, &response);
        ASSERT_TRUE(status.ok()) << "Failed to create job: " << status.error_message();
        ASSERT_TRUE(response.success()) << "Job creation failed: " << response.message();

        job_ids.push_back(response.job_id());
        // Don't add to cleanup list - we want to check if they persist
        LOG(INFO) << "Created job for persistence test: " << response.job_id();
    }

    // Verify jobs exist before restart
    {
        auto get_stub = swdv::ifex_scheduler::get_jobs_service::NewStub(scheduler_channel_);
        swdv::ifex_scheduler::get_jobs_request request;
        swdv::ifex_scheduler::get_jobs_response response;
        grpc::ClientContext context;

        auto status = get_stub->get_jobs(&context, request, &response);
        ASSERT_TRUE(status.ok());

        int found = 0;
        for (const auto& job : response.jobs()) {
            if (job.title().find("Persistence Test Job") != std::string::npos) {
                found++;
            }
        }
        EXPECT_EQ(found, 3) << "Should have 3 persistence test jobs before restart";
    }

    // Restart the scheduler (this will lose all jobs until persistence is implemented)
    LOG(INFO) << "=== Restarting scheduler to test persistence ===";
    ASSERT_TRUE(RestartScheduler()) << "Failed to restart scheduler";

    // Reconnect to scheduler
    scheduler_channel_ = grpc::CreateChannel(TEST_SCHEDULER_ADDRESS, grpc::InsecureChannelCredentials());

    // Verify jobs still exist after restart
    // THIS WILL FAIL UNTIL PERSISTENCE IS IMPLEMENTED
    {
        auto get_stub = swdv::ifex_scheduler::get_jobs_service::NewStub(scheduler_channel_);
        swdv::ifex_scheduler::get_jobs_request request;
        swdv::ifex_scheduler::get_jobs_response response;
        grpc::ClientContext context;

        auto status = get_stub->get_jobs(&context, request, &response);
        ASSERT_TRUE(status.ok());

        int found = 0;
        for (const auto& job : response.jobs()) {
            if (job.title().find("Persistence Test Job") != std::string::npos) {
                found++;
            }
        }
        EXPECT_EQ(found, 3)
            << "Jobs should survive scheduler restart. Found " << found << " jobs, expected 3. "
            << "This test fails until persistence is implemented.";
    }

    // Clean up (delete jobs if they exist)
    for (const auto& job_id : job_ids) {
        delete_job(job_id);
    }
}

// =============================================================================
// Pause/Resume/Trigger Tests
// =============================================================================

TEST_F(SchedulerIntegrationTest, PauseJob) {
    // Create a job
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::create_job_request create_request;
    auto* job = create_request.mutable_job();
    job->set_title("Job to Pause");
    job->set_service("echo_service");
    job->set_method("echo");
    job->set_parameters(R"({"message": "pause me"})");
    job->set_scheduled_time_ms(get_future_time_ms(3600));  // Far future

    swdv::ifex_scheduler::create_job_response create_response;
    grpc::ClientContext create_context;

    auto status = create_stub->create_job(&create_context, create_request, &create_response);
    ASSERT_TRUE(status.ok() && create_response.success());

    std::string job_id = create_response.job_id();
    created_job_ids_.push_back(job_id);
    LOG(INFO) << "Created job to pause: " << job_id;

    // Verify job is PENDING
    auto get_stub = swdv::ifex_scheduler::get_job_service::NewStub(scheduler_channel_);
    {
        swdv::ifex_scheduler::get_job_request get_request;
        get_request.set_job_id(job_id);
        swdv::ifex_scheduler::get_job_response get_response;
        grpc::ClientContext get_context;

        status = get_stub->get_job(&get_context, get_request, &get_response);
        ASSERT_TRUE(status.ok() && get_response.success());
        EXPECT_EQ(get_response.job().status(), swdv::scheduler_types::JOB_STATUS_PENDING);
    }

    // Pause the job
    auto pause_stub = swdv::ifex_scheduler::pause_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::pause_job_request pause_request;
    pause_request.set_job_id(job_id);

    swdv::ifex_scheduler::pause_job_response pause_response;
    grpc::ClientContext pause_context;

    status = pause_stub->pause_job(&pause_context, pause_request, &pause_response);
    ASSERT_TRUE(status.ok()) << "Failed to pause job: " << status.error_message();
    EXPECT_TRUE(pause_response.success()) << "Pause failed: " << pause_response.message();

    // Verify job is now PAUSED
    {
        swdv::ifex_scheduler::get_job_request get_request;
        get_request.set_job_id(job_id);
        swdv::ifex_scheduler::get_job_response get_response;
        grpc::ClientContext get_context;

        status = get_stub->get_job(&get_context, get_request, &get_response);
        ASSERT_TRUE(status.ok() && get_response.success());
        EXPECT_TRUE(get_response.job().paused())
            << "Job should be paused after pause_job call";
    }
}

TEST_F(SchedulerIntegrationTest, PauseJobInvalidStatus) {
    // Create a job
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::create_job_request create_request;
    auto* job = create_request.mutable_job();
    job->set_title("Job for Invalid Pause");
    job->set_service("echo_service");
    job->set_method("echo");
    job->set_parameters(R"({"message": "invalid pause"})");
    job->set_scheduled_time_ms(get_future_time_ms(3600));

    swdv::ifex_scheduler::create_job_response create_response;
    grpc::ClientContext create_context;

    auto status = create_stub->create_job(&create_context, create_request, &create_response);
    ASSERT_TRUE(status.ok() && create_response.success());

    std::string job_id = create_response.job_id();
    created_job_ids_.push_back(job_id);

    // Pause it first
    auto pause_stub = swdv::ifex_scheduler::pause_job_service::NewStub(scheduler_channel_);
    {
        swdv::ifex_scheduler::pause_job_request pause_request;
        pause_request.set_job_id(job_id);
        swdv::ifex_scheduler::pause_job_response pause_response;
        grpc::ClientContext pause_context;

        status = pause_stub->pause_job(&pause_context, pause_request, &pause_response);
        ASSERT_TRUE(status.ok() && pause_response.success());
    }

    // Try to pause again - should fail since it's already PAUSED
    {
        swdv::ifex_scheduler::pause_job_request pause_request;
        pause_request.set_job_id(job_id);
        swdv::ifex_scheduler::pause_job_response pause_response;
        grpc::ClientContext pause_context;

        status = pause_stub->pause_job(&pause_context, pause_request, &pause_response);
        ASSERT_TRUE(status.ok());
        EXPECT_FALSE(pause_response.success())
            << "Should not be able to pause an already paused job";
        EXPECT_FALSE(pause_response.message().empty())
            << "Should have error message explaining why pause failed";
    }
}

TEST_F(SchedulerIntegrationTest, ResumeJob) {
    // Create a job
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::create_job_request create_request;
    auto* job = create_request.mutable_job();
    job->set_title("Job to Resume");
    job->set_service("echo_service");
    job->set_method("echo");
    job->set_parameters(R"({"message": "resume me"})");
    job->set_scheduled_time_ms(get_future_time_ms(3600));

    swdv::ifex_scheduler::create_job_response create_response;
    grpc::ClientContext create_context;

    auto status = create_stub->create_job(&create_context, create_request, &create_response);
    ASSERT_TRUE(status.ok() && create_response.success());

    std::string job_id = create_response.job_id();
    created_job_ids_.push_back(job_id);
    LOG(INFO) << "Created job to resume: " << job_id;

    // Pause the job first
    auto pause_stub = swdv::ifex_scheduler::pause_job_service::NewStub(scheduler_channel_);
    {
        swdv::ifex_scheduler::pause_job_request pause_request;
        pause_request.set_job_id(job_id);
        swdv::ifex_scheduler::pause_job_response pause_response;
        grpc::ClientContext pause_context;

        status = pause_stub->pause_job(&pause_context, pause_request, &pause_response);
        ASSERT_TRUE(status.ok() && pause_response.success());
    }

    // Resume the job
    auto resume_stub = swdv::ifex_scheduler::resume_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::resume_job_request resume_request;
    resume_request.set_job_id(job_id);

    swdv::ifex_scheduler::resume_job_response resume_response;
    grpc::ClientContext resume_context;

    status = resume_stub->resume_job(&resume_context, resume_request, &resume_response);
    ASSERT_TRUE(status.ok()) << "Failed to resume job: " << status.error_message();
    EXPECT_TRUE(resume_response.success()) << "Resume failed: " << resume_response.message();

    // Verify job is now PENDING again
    auto get_stub = swdv::ifex_scheduler::get_job_service::NewStub(scheduler_channel_);
    {
        swdv::ifex_scheduler::get_job_request get_request;
        get_request.set_job_id(job_id);
        swdv::ifex_scheduler::get_job_response get_response;
        grpc::ClientContext get_context;

        status = get_stub->get_job(&get_context, get_request, &get_response);
        ASSERT_TRUE(status.ok() && get_response.success());
        EXPECT_EQ(get_response.job().status(), swdv::scheduler_types::JOB_STATUS_PENDING)
            << "Job should be PENDING after resume_job call";
    }
}

TEST_F(SchedulerIntegrationTest, ResumeJobInvalidStatus) {
    // Create a job (starts as PENDING, not PAUSED)
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::create_job_request create_request;
    auto* job = create_request.mutable_job();
    job->set_title("Job for Invalid Resume");
    job->set_service("echo_service");
    job->set_method("echo");
    job->set_parameters(R"({"message": "invalid resume"})");
    job->set_scheduled_time_ms(get_future_time_ms(3600));

    swdv::ifex_scheduler::create_job_response create_response;
    grpc::ClientContext create_context;

    auto status = create_stub->create_job(&create_context, create_request, &create_response);
    ASSERT_TRUE(status.ok() && create_response.success());

    std::string job_id = create_response.job_id();
    created_job_ids_.push_back(job_id);

    // Try to resume a PENDING job - should fail
    auto resume_stub = swdv::ifex_scheduler::resume_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::resume_job_request resume_request;
    resume_request.set_job_id(job_id);

    swdv::ifex_scheduler::resume_job_response resume_response;
    grpc::ClientContext resume_context;

    status = resume_stub->resume_job(&resume_context, resume_request, &resume_response);
    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(resume_response.success())
        << "Should not be able to resume a job that is not paused";
    EXPECT_FALSE(resume_response.message().empty())
        << "Should have error message explaining why resume failed";
}

TEST_F(SchedulerIntegrationTest, TriggerJob) {
    // Create a job scheduled far in the future
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::create_job_request create_request;
    auto* job = create_request.mutable_job();
    job->set_title("Job to Trigger");
    job->set_service("echo_service");
    job->set_method("echo");
    job->set_parameters(R"({"message": "triggered execution"})");
    job->set_scheduled_time_ms(get_future_time_ms(86400));  // Tomorrow

    swdv::ifex_scheduler::create_job_response create_response;
    grpc::ClientContext create_context;

    auto status = create_stub->create_job(&create_context, create_request, &create_response);
    ASSERT_TRUE(status.ok() && create_response.success());

    std::string job_id = create_response.job_id();
    created_job_ids_.push_back(job_id);
    LOG(INFO) << "Created job to trigger: " << job_id;

    // Trigger immediate execution
    auto trigger_stub = swdv::ifex_scheduler::trigger_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::trigger_job_request trigger_request;
    trigger_request.set_job_id(job_id);

    swdv::ifex_scheduler::trigger_job_response trigger_response;
    grpc::ClientContext trigger_context;

    status = trigger_stub->trigger_job(&trigger_context, trigger_request, &trigger_response);
    ASSERT_TRUE(status.ok()) << "Failed to trigger job: " << status.error_message();
    EXPECT_TRUE(trigger_response.success()) << "Trigger failed: " << trigger_response.message();

    // Response should contain the updated job with execution result
    if (trigger_response.has_job()) {
        const auto& triggered_job = trigger_response.job();
        LOG(INFO) << "Triggered job status: " << triggered_job.status();
        // Job should have been executed (either COMPLETED or FAILED)
        EXPECT_TRUE(triggered_job.status() == swdv::scheduler_types::JOB_STATUS_COMPLETED ||
                    triggered_job.status() == swdv::scheduler_types::JOB_STATUS_FAILED)
            << "Triggered job should be in terminal state";
    }

    // Verify via get_job that execution happened
    auto get_stub = swdv::ifex_scheduler::get_job_service::NewStub(scheduler_channel_);
    {
        swdv::ifex_scheduler::get_job_request get_request;
        get_request.set_job_id(job_id);
        swdv::ifex_scheduler::get_job_response get_response;
        grpc::ClientContext get_context;

        status = get_stub->get_job(&get_context, get_request, &get_response);
        ASSERT_TRUE(status.ok() && get_response.success());

        // Should have executed_at_ms timestamp (non-zero)
        EXPECT_GT(get_response.job().executed_at_ms(), 0u)
            << "Triggered job should have executed_at_ms timestamp";
    }
}

TEST_F(SchedulerIntegrationTest, TriggerPausedJob) {
    // Create and pause a job
    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::create_job_request create_request;
    auto* job = create_request.mutable_job();
    job->set_title("Paused Job to Trigger");
    job->set_service("echo_service");
    job->set_method("echo");
    job->set_parameters(R"({"message": "trigger paused"})");
    job->set_scheduled_time_ms(get_future_time_ms(86400));

    swdv::ifex_scheduler::create_job_response create_response;
    grpc::ClientContext create_context;

    auto status = create_stub->create_job(&create_context, create_request, &create_response);
    ASSERT_TRUE(status.ok() && create_response.success());

    std::string job_id = create_response.job_id();
    created_job_ids_.push_back(job_id);

    // Pause the job
    auto pause_stub = swdv::ifex_scheduler::pause_job_service::NewStub(scheduler_channel_);
    {
        swdv::ifex_scheduler::pause_job_request pause_request;
        pause_request.set_job_id(job_id);
        swdv::ifex_scheduler::pause_job_response pause_response;
        grpc::ClientContext pause_context;

        status = pause_stub->pause_job(&pause_context, pause_request, &pause_response);
        ASSERT_TRUE(status.ok() && pause_response.success());
    }

    // Trigger the paused job - should still work
    auto trigger_stub = swdv::ifex_scheduler::trigger_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::trigger_job_request trigger_request;
    trigger_request.set_job_id(job_id);

    swdv::ifex_scheduler::trigger_job_response trigger_response;
    grpc::ClientContext trigger_context;

    status = trigger_stub->trigger_job(&trigger_context, trigger_request, &trigger_response);
    ASSERT_TRUE(status.ok()) << "Failed to trigger paused job: " << status.error_message();
    EXPECT_TRUE(trigger_response.success())
        << "Should be able to trigger a paused job: " << trigger_response.message();
}

TEST_F(SchedulerIntegrationTest, PauseResumeWorkflow) {
    // Test full workflow: create -> pause -> resume -> pause -> trigger

    auto create_stub = swdv::ifex_scheduler::create_job_service::NewStub(scheduler_channel_);
    auto get_stub = swdv::ifex_scheduler::get_job_service::NewStub(scheduler_channel_);
    auto pause_stub = swdv::ifex_scheduler::pause_job_service::NewStub(scheduler_channel_);
    auto resume_stub = swdv::ifex_scheduler::resume_job_service::NewStub(scheduler_channel_);
    auto trigger_stub = swdv::ifex_scheduler::trigger_job_service::NewStub(scheduler_channel_);

    // Create job
    swdv::ifex_scheduler::create_job_request create_request;
    auto* job = create_request.mutable_job();
    job->set_title("Workflow Test Job");
    job->set_service("echo_service");
    job->set_method("echo");
    job->set_parameters(R"({"message": "workflow test"})");
    job->set_scheduled_time_ms(get_future_time_ms(86400));

    swdv::ifex_scheduler::create_job_response create_response;
    grpc::ClientContext create_context;

    auto status = create_stub->create_job(&create_context, create_request, &create_response);
    ASSERT_TRUE(status.ok() && create_response.success());

    std::string job_id = create_response.job_id();
    created_job_ids_.push_back(job_id);
    LOG(INFO) << "Workflow test job: " << job_id;

    // Helper to get current status
    auto get_status = [&]() -> swdv::scheduler_types::job_status_t {
        swdv::ifex_scheduler::get_job_request req;
        req.set_job_id(job_id);
        swdv::ifex_scheduler::get_job_response resp;
        grpc::ClientContext ctx;
        get_stub->get_job(&ctx, req, &resp);
        return resp.job().status();
    };

    // Helper to get paused state
    auto get_paused = [&]() -> bool {
        swdv::ifex_scheduler::get_job_request req;
        req.set_job_id(job_id);
        swdv::ifex_scheduler::get_job_response resp;
        grpc::ClientContext ctx;
        get_stub->get_job(&ctx, req, &resp);
        return resp.job().paused();
    };

    // Step 1: Initial status should be PENDING and not paused
    EXPECT_EQ(get_status(), swdv::scheduler_types::JOB_STATUS_PENDING) << "Initial status should be PENDING";
    EXPECT_FALSE(get_paused()) << "Initial job should not be paused";

    // Step 2: Pause -> paused=true (status unchanged)
    {
        swdv::ifex_scheduler::pause_job_request req;
        req.set_job_id(job_id);
        swdv::ifex_scheduler::pause_job_response resp;
        grpc::ClientContext ctx;
        status = pause_stub->pause_job(&ctx, req, &resp);
        ASSERT_TRUE(status.ok() && resp.success());
    }
    EXPECT_TRUE(get_paused()) << "Job should be paused after pause";

    // Step 3: Resume -> paused=false
    {
        swdv::ifex_scheduler::resume_job_request req;
        req.set_job_id(job_id);
        swdv::ifex_scheduler::resume_job_response resp;
        grpc::ClientContext ctx;
        status = resume_stub->resume_job(&ctx, req, &resp);
        ASSERT_TRUE(status.ok() && resp.success());
    }
    EXPECT_FALSE(get_paused()) << "Job should not be paused after resume";

    // Step 4: Pause again -> paused=true
    {
        swdv::ifex_scheduler::pause_job_request req;
        req.set_job_id(job_id);
        swdv::ifex_scheduler::pause_job_response resp;
        grpc::ClientContext ctx;
        status = pause_stub->pause_job(&ctx, req, &resp);
        ASSERT_TRUE(status.ok() && resp.success());
    }
    EXPECT_TRUE(get_paused()) << "Job should be paused after second pause";

    // Step 5: Trigger -> COMPLETED or FAILED
    {
        swdv::ifex_scheduler::trigger_job_request req;
        req.set_job_id(job_id);
        swdv::ifex_scheduler::trigger_job_response resp;
        grpc::ClientContext ctx;
        status = trigger_stub->trigger_job(&ctx, req, &resp);
        ASSERT_TRUE(status.ok() && resp.success());
    }
    auto final_status = get_status();
    EXPECT_TRUE(final_status == swdv::scheduler_types::JOB_STATUS_COMPLETED ||
                final_status == swdv::scheduler_types::JOB_STATUS_FAILED)
        << "Status should be terminal after trigger";

    LOG(INFO) << "Workflow complete. Final status: " << final_status;
}

TEST_F(SchedulerIntegrationTest, PauseNonExistentJob) {
    auto pause_stub = swdv::ifex_scheduler::pause_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::pause_job_request request;
    request.set_job_id("non-existent-job-id-12345");

    swdv::ifex_scheduler::pause_job_response response;
    grpc::ClientContext context;

    auto status = pause_stub->pause_job(&context, request, &response);
    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(response.success()) << "Should fail for non-existent job";
    EXPECT_FALSE(response.message().empty()) << "Should have error message";
}

TEST_F(SchedulerIntegrationTest, ResumeNonExistentJob) {
    auto resume_stub = swdv::ifex_scheduler::resume_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::resume_job_request request;
    request.set_job_id("non-existent-job-id-12345");

    swdv::ifex_scheduler::resume_job_response response;
    grpc::ClientContext context;

    auto status = resume_stub->resume_job(&context, request, &response);
    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(response.success()) << "Should fail for non-existent job";
    EXPECT_FALSE(response.message().empty()) << "Should have error message";
}

TEST_F(SchedulerIntegrationTest, TriggerNonExistentJob) {
    auto trigger_stub = swdv::ifex_scheduler::trigger_job_service::NewStub(scheduler_channel_);

    swdv::ifex_scheduler::trigger_job_request request;
    request.set_job_id("non-existent-job-id-12345");

    swdv::ifex_scheduler::trigger_job_response response;
    grpc::ClientContext context;

    auto status = trigger_stub->trigger_job(&context, request, &response);
    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(response.success()) << "Should fail for non-existent job";
    EXPECT_FALSE(response.message().empty()) << "Should have error message";
}
