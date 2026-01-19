// Job hash tests

#include "job.hpp"
#include "job_hash.hpp"
#include <gtest/gtest.h>
#include <algorithm>

using namespace ifex::scheduler;

class JobHashTest : public ::testing::Test {
protected:
    Job create_test_job(const std::string& id = "job-1") {
        Job job;
        job.job_id = id;
        job.vehicle_id = "VIN001";
        job.title = "Test Job";
        job.service = "test_service";
        job.method = "test_method";
        job.parameters_json = R"({"key": "value"})";
        job.scheduled_time_ms = 1700000000000;
        job.recurrence_rule = "";
        job.end_time_ms = 0;
        job.paused = false;
        job.wake_policy = WakePolicy::NO_WAKE;
        job.sleep_policy = SleepPolicy::NORMAL;
        job.wake_lead_time_s = 0;
        job.status = JobStatus::PENDING;
        job.created_at_ms = 1699999000000;
        job.updated_at_ms = 1699999000000;
        return job;
    }
};

TEST_F(JobHashTest, SameJobSameHash) {
    Job job1 = create_test_job();
    Job job2 = create_test_job();

    EXPECT_EQ(compute_job_content_hash(job1), compute_job_content_hash(job2));
    EXPECT_EQ(job1.content_hash(), job2.content_hash());
}

TEST_F(JobHashTest, DifferentJobIdDifferentHash) {
    Job job1 = create_test_job("job-1");
    Job job2 = create_test_job("job-2");

    EXPECT_NE(compute_job_content_hash(job1), compute_job_content_hash(job2));
}

TEST_F(JobHashTest, DifferentTitleDifferentHash) {
    Job job1 = create_test_job();
    Job job2 = create_test_job();
    job2.title = "Different Title";

    EXPECT_NE(compute_job_content_hash(job1), compute_job_content_hash(job2));
}

TEST_F(JobHashTest, DifferentScheduleTimeDifferentHash) {
    Job job1 = create_test_job();
    Job job2 = create_test_job();
    job2.scheduled_time_ms = job1.scheduled_time_ms + 1000;

    EXPECT_NE(compute_job_content_hash(job1), compute_job_content_hash(job2));
}

TEST_F(JobHashTest, StatusNotIncludedInHash) {
    Job job1 = create_test_job();
    Job job2 = create_test_job();
    job2.status = JobStatus::COMPLETED;

    EXPECT_EQ(compute_job_content_hash(job1), compute_job_content_hash(job2));
}

TEST_F(JobHashTest, MetadataNotIncludedInHash) {
    Job job1 = create_test_job();
    Job job2 = create_test_job();
    job2.created_at_ms = job1.created_at_ms + 1000;
    job2.updated_at_ms = job1.updated_at_ms + 1000;

    EXPECT_EQ(compute_job_content_hash(job1), compute_job_content_hash(job2));
}

TEST_F(JobHashTest, VersionNotIncludedInHash) {
    Job job1 = create_test_job();
    Job job2 = create_test_job();
    job2.version.cloud_seq = 10;
    job2.version.vehicle_seq = 5;

    EXPECT_EQ(compute_job_content_hash(job1), compute_job_content_hash(job2));
}

TEST_F(JobHashTest, PausedIncludedInHash) {
    Job job1 = create_test_job();
    Job job2 = create_test_job();
    job2.paused = true;

    EXPECT_NE(compute_job_content_hash(job1), compute_job_content_hash(job2));
}

TEST_F(JobHashTest, WakePolicyIncludedInHash) {
    Job job1 = create_test_job();
    Job job2 = create_test_job();
    job2.wake_policy = WakePolicy::WAKE_REQUIRED;

    EXPECT_NE(compute_job_content_hash(job1), compute_job_content_hash(job2));
}

TEST_F(JobHashTest, StateChecksumDeterministic) {
    std::vector<Job> jobs;
    jobs.push_back(create_test_job("job-1"));
    jobs.push_back(create_test_job("job-2"));
    jobs.push_back(create_test_job("job-3"));

    // Sort by job_id
    std::sort(jobs.begin(), jobs.end(),
              [](const Job& a, const Job& b) { return a.job_id < b.job_id; });

    uint64_t checksum1 = compute_state_checksum(jobs);
    uint64_t checksum2 = compute_state_checksum(jobs);

    EXPECT_EQ(checksum1, checksum2);
}

TEST_F(JobHashTest, StateChecksumChangesWithJob) {
    std::vector<Job> jobs1;
    jobs1.push_back(create_test_job("job-1"));
    jobs1.push_back(create_test_job("job-2"));
    std::sort(jobs1.begin(), jobs1.end(),
              [](const Job& a, const Job& b) { return a.job_id < b.job_id; });

    std::vector<Job> jobs2 = jobs1;
    jobs2[0].title = "Modified Title";

    EXPECT_NE(compute_state_checksum(jobs1), compute_state_checksum(jobs2));
}

TEST_F(JobHashTest, EmptyJobsSeedChecksum) {
    // Empty state returns the xxHash64 seed value for consistency with
    // the original scheduler_sync_bridge implementation
    constexpr uint64_t XXHASH64_SEED = 0x9e3779b97f4a7c15ULL;
    std::vector<Job> empty;
    EXPECT_EQ(compute_state_checksum(empty), XXHASH64_SEED);
}
