// Memory store tests

#include "memory_store.hpp"
#include <gtest/gtest.h>

using namespace ifex::scheduler;

class MemoryStoreTest : public ::testing::Test {
protected:
    Job create_test_job(const std::string& vehicle_id, const std::string& job_id) {
        Job job;
        job.job_id = job_id;
        job.vehicle_id = vehicle_id;
        job.title = "Test Job " + job_id;
        job.service = "test_service";
        job.method = "test_method";
        job.parameters_json = "{}";
        job.scheduled_time_ms = 1700000000000;
        job.status = JobStatus::PENDING;
        job.created_at_ms = 1699999000000;
        job.updated_at_ms = 1699999000000;
        return job;
    }

    MemoryStore store_;
};

TEST_F(MemoryStoreTest, PutAndGet) {
    Job job = create_test_job("VIN001", "job-1");
    store_.put(job);

    auto retrieved = store_.get("VIN001", "job-1");
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->job_id, "job-1");
    EXPECT_EQ(retrieved->title, "Test Job job-1");
}

TEST_F(MemoryStoreTest, GetNonExistent) {
    auto retrieved = store_.get("VIN001", "nonexistent");
    EXPECT_FALSE(retrieved.has_value());
}

TEST_F(MemoryStoreTest, Update) {
    Job job = create_test_job("VIN001", "job-1");
    store_.put(job);

    job.title = "Updated Title";
    store_.put(job);

    auto retrieved = store_.get("VIN001", "job-1");
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->title, "Updated Title");
}

TEST_F(MemoryStoreTest, SoftDelete) {
    Job job = create_test_job("VIN001", "job-1");
    store_.put(job);

    store_.remove("VIN001", "job-1", true);

    // Should still exist but marked deleted
    auto all_jobs = store_.get_all("VIN001", true);
    ASSERT_EQ(all_jobs.size(), 1);
    EXPECT_TRUE(all_jobs[0].deleted);
    EXPECT_GT(all_jobs[0].deleted_at_ms, 0);

    // Should not appear in non-deleted list
    auto active_jobs = store_.get_all("VIN001", false);
    EXPECT_EQ(active_jobs.size(), 0);
}

TEST_F(MemoryStoreTest, HardDelete) {
    Job job = create_test_job("VIN001", "job-1");
    store_.put(job);

    store_.remove("VIN001", "job-1", false);

    auto all_jobs = store_.get_all("VIN001", true);
    EXPECT_EQ(all_jobs.size(), 0);
}

TEST_F(MemoryStoreTest, ListWithFilter) {
    store_.put(create_test_job("VIN001", "job-1"));
    store_.put(create_test_job("VIN001", "job-2"));
    store_.put(create_test_job("VIN002", "job-3"));

    JobFilter filter;
    filter.vehicle_id = "VIN001";

    auto result = store_.list(filter);
    EXPECT_EQ(result.jobs.size(), 2);
    EXPECT_EQ(result.total_count, 2);
}

TEST_F(MemoryStoreTest, ListWithStatusFilter) {
    auto job1 = create_test_job("VIN001", "job-1");
    job1.status = JobStatus::PENDING;
    store_.put(job1);

    auto job2 = create_test_job("VIN001", "job-2");
    job2.status = JobStatus::COMPLETED;
    store_.put(job2);

    JobFilter filter;
    filter.vehicle_id = "VIN001";
    filter.status = JobStatus::PENDING;

    auto result = store_.list(filter);
    EXPECT_EQ(result.jobs.size(), 1);
    EXPECT_EQ(result.jobs[0].job_id, "job-1");
}

TEST_F(MemoryStoreTest, DirtyTracking) {
    MemoryStoreConfig config;
    config.track_dirty = true;
    MemoryStore store(config);

    Job job = create_test_job("VIN001", "job-1");
    store.put(job);

    EXPECT_TRUE(store.has_pending_sync("VIN001"));

    auto pending = store.get_pending_sync("VIN001");
    EXPECT_EQ(pending.size(), 1);

    store.mark_synced("VIN001", {"job-1"});
    EXPECT_FALSE(store.has_pending_sync("VIN001"));
}

TEST_F(MemoryStoreTest, StateChecksum) {
    store_.put(create_test_job("VIN001", "job-1"));
    store_.put(create_test_job("VIN001", "job-2"));

    uint64_t checksum1 = store_.state_checksum("VIN001");
    uint64_t checksum2 = store_.state_checksum("VIN001");

    EXPECT_EQ(checksum1, checksum2);
    EXPECT_NE(checksum1, 0);

    // Modify and checksum should change
    auto job = create_test_job("VIN001", "job-1");
    job.title = "Modified";
    store_.put(job);

    uint64_t checksum3 = store_.state_checksum("VIN001");
    EXPECT_NE(checksum1, checksum3);
}

TEST_F(MemoryStoreTest, PurgeTombstones) {
    // Create and delete a job
    store_.put(create_test_job("VIN001", "job-1"));
    store_.remove("VIN001", "job-1", true);

    // Should have 1 tombstone
    auto stats = store_.get_stats();
    EXPECT_EQ(stats.tombstones, 1);

    // Purge with 0 retention should remove it
    int purged = store_.purge_tombstones(0);
    EXPECT_EQ(purged, 1);

    stats = store_.get_stats();
    EXPECT_EQ(stats.tombstones, 0);
}

TEST_F(MemoryStoreTest, GetStats) {
    store_.put(create_test_job("VIN001", "job-1"));
    store_.put(create_test_job("VIN001", "job-2"));
    store_.put(create_test_job("VIN002", "job-3"));

    auto stats = store_.get_stats();
    EXPECT_EQ(stats.total_jobs, 3);
    EXPECT_EQ(stats.total_vehicles, 2);
}

TEST_F(MemoryStoreTest, Clear) {
    store_.put(create_test_job("VIN001", "job-1"));
    store_.put(create_test_job("VIN002", "job-2"));

    store_.clear();

    auto stats = store_.get_stats();
    EXPECT_EQ(stats.total_jobs, 0);
    EXPECT_EQ(stats.total_vehicles, 0);
}
