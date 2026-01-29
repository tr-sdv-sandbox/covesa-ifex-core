// Proto Adapter - Convert between Job and protobuf wire format
//
// This header provides conversion functions between the canonical Job
// structure and the scheduler_sync_v3.proto JobRecord message.
//
// IMPORTANT: This file requires the proto-generated headers to be included
// first. It is intentionally header-only to avoid library dependency issues.
//
// Usage:
//   #include "scheduler_sync_v3.pb.h"  // Proto-generated
//   #include "proto_adapter.hpp"       // This file
//
//   Job job = from_proto(job_record);
//   to_proto(job, &job_record);

#pragma once

#include "../include/job.hpp"

// Forward declare proto types (user must include the actual headers)
namespace swdv::scheduler_sync_v3 {
    class JobRecord;
    enum JobStatus : int;
    enum WakePolicy : int;
    enum SleepPolicy : int;
    enum JobAuthority : int;
}

namespace ifex::scheduler {

// Convert proto JobStatus to our JobStatus
inline JobStatus job_status_from_proto(int proto_status) {
    // Proto enum values: PENDING=0, RUNNING=1, COMPLETED=2, FAILED=3, CANCELLED=4
    switch (proto_status) {
        case 0: return JobStatus::PENDING;
        case 1: return JobStatus::RUNNING;
        case 2: return JobStatus::COMPLETED;
        case 3: return JobStatus::FAILED;
        case 4: return JobStatus::CANCELLED;
        default: return JobStatus::PENDING;
    }
}

// Convert our JobStatus to proto
inline int job_status_to_proto(JobStatus status) {
    return static_cast<int>(status);
}

// Convert proto WakePolicy to our WakePolicy
inline WakePolicy wake_policy_from_proto(int proto_policy) {
    return proto_policy == 1 ? WakePolicy::WAKE_REQUIRED : WakePolicy::NO_WAKE;
}

// Convert our WakePolicy to proto
inline int wake_policy_to_proto(WakePolicy policy) {
    return static_cast<int>(policy);
}

// Convert proto SleepPolicy to our SleepPolicy
inline SleepPolicy sleep_policy_from_proto(int proto_policy) {
    return proto_policy == 1 ? SleepPolicy::INHIBIT : SleepPolicy::NORMAL;
}

// Convert our SleepPolicy to proto
inline int sleep_policy_to_proto(SleepPolicy policy) {
    return static_cast<int>(policy);
}

// Convert proto JobAuthority to our JobAuthority
inline JobAuthority job_authority_from_proto(int proto_authority) {
    return proto_authority == 1 ? JobAuthority::VEHICLE : JobAuthority::CLOUD;
}

// Convert our JobAuthority to proto
inline int job_authority_to_proto(JobAuthority authority) {
    return static_cast<int>(authority);
}

// Convert from proto JobRecord to Job
// Template to work with any proto message that has the expected fields
template<typename ProtoJobRecord>
Job from_proto(const ProtoJobRecord& record) {
    Job job;

    job.job_id = record.job_id();
    // vehicle_id is not in JobRecord - must be set separately
    job.title = record.title();
    job.service = record.service();
    job.method = record.method();
    job.parameters_json = record.parameters_json();
    job.scheduled_time_ms = record.scheduled_time_ms();
    job.recurrence_rule = record.recurrence_rule();
    job.end_time_ms = record.end_time_ms();
    job.paused = record.paused();
    job.wake_policy = wake_policy_from_proto(static_cast<int>(record.wake_policy()));
    job.sleep_policy = sleep_policy_from_proto(static_cast<int>(record.sleep_policy()));
    job.wake_lead_time_s = record.wake_lead_time_s();

    job.status = job_status_from_proto(static_cast<int>(record.status()));
    job.next_run_time_ms = record.next_run_time_ms();

    job.created_at_ms = record.created_at_ms();
    job.updated_at_ms = record.updated_at_ms();

    job.local_version.cloud_seq = record.version().cloud_seq();
    job.local_version.vehicle_seq = record.version().vehicle_seq();
    job.authority = job_authority_from_proto(static_cast<int>(record.authority()));

    job.deleted = record.deleted();
    job.deleted_at_ms = record.deleted_at_ms();

    return job;
}

// Convert from Job to proto JobRecord
// Template to work with any proto message that has the expected fields
template<typename ProtoJobRecord>
void to_proto(const Job& job, ProtoJobRecord* record) {
    record->set_job_id(job.job_id);
    record->set_title(job.title);
    record->set_service(job.service);
    record->set_method(job.method);
    record->set_parameters_json(job.parameters_json.empty() ? "{}" : job.parameters_json);
    record->set_scheduled_time_ms(job.scheduled_time_ms);
    record->set_recurrence_rule(job.recurrence_rule);
    if (job.end_time_ms > 0) {
        record->set_end_time_ms(job.end_time_ms);
    }
    record->set_paused(job.paused);

    // Set enums using the proto's enum type (cast from int)
    record->set_wake_policy(
        static_cast<typename std::remove_reference<decltype(record->wake_policy())>::type>(
            wake_policy_to_proto(job.wake_policy)));
    record->set_sleep_policy(
        static_cast<typename std::remove_reference<decltype(record->sleep_policy())>::type>(
            sleep_policy_to_proto(job.sleep_policy)));
    record->set_wake_lead_time_s(job.wake_lead_time_s);

    record->set_status(
        static_cast<typename std::remove_reference<decltype(record->status())>::type>(
            job_status_to_proto(job.status)));
    if (job.next_run_time_ms > 0) {
        record->set_next_run_time_ms(job.next_run_time_ms);
    }

    record->set_created_at_ms(job.created_at_ms);
    record->set_updated_at_ms(job.updated_at_ms);

    auto* version = record->mutable_version();
    version->set_cloud_seq(job.local_version.cloud_seq);
    version->set_vehicle_seq(job.local_version.vehicle_seq);

    record->set_authority(
        static_cast<typename std::remove_reference<decltype(record->authority())>::type>(
            job_authority_to_proto(job.authority)));

    record->set_deleted(job.deleted);
    if (job.deleted_at_ms > 0) {
        record->set_deleted_at_ms(job.deleted_at_ms);
    }
}

}  // namespace ifex::scheduler
