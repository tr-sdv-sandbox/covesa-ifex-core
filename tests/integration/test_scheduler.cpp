#include <gtest/gtest.h>
#include <glog/logging.h>
#include "ifex-scheduler-service.grpc.pb.h"
#include "service-discovery-service.grpc.pb.h"
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

    std::string get_current_time() {
        return get_future_time(0);
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
    job->set_scheduled_time(get_future_time(3600));

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
    EXPECT_EQ(retrieved_job.status(), swdv::ifex_scheduler::PENDING);
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
        job->set_scheduled_time(get_future_time(3600 + i));

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
    job->set_scheduled_time(get_future_time(3600));

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
    job->set_scheduled_time(get_future_time(3600));

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
        job->set_scheduled_time(get_future_time(60 * (i + 1)));

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
    request.set_date(get_current_time());

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
    job->set_scheduled_time(get_future_time(3600));

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
    job->set_scheduled_time(get_future_time(3600));
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
