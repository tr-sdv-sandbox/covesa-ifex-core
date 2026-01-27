// Job implementation

#include "job.hpp"
#include "job_hash.hpp"

namespace ifex::scheduler {

uint64_t Job::content_hash() const {
    return compute_job_content_hash(*this);
}

const char* job_status_to_string(JobStatus status) {
    switch (status) {
        case JobStatus::PENDING: return "pending";
        case JobStatus::RUNNING: return "running";
        case JobStatus::COMPLETED: return "completed";
        case JobStatus::FAILED: return "failed";
        case JobStatus::CANCELLED: return "cancelled";
    }
    return "pending";
}

const char* wake_policy_to_string(WakePolicy policy) {
    switch (policy) {
        case WakePolicy::NO_WAKE: return "no_wake";
        case WakePolicy::WAKE_REQUIRED: return "wake_required";
    }
    return "no_wake";
}

const char* sleep_policy_to_string(SleepPolicy policy) {
    switch (policy) {
        case SleepPolicy::NORMAL: return "normal";
        case SleepPolicy::INHIBIT: return "inhibit";
    }
    return "normal";
}

const char* job_authority_to_string(JobAuthority authority) {
    switch (authority) {
        case JobAuthority::CLOUD: return "cloud";
        case JobAuthority::VEHICLE: return "vehicle";
    }
    return "cloud";
}

JobStatus job_status_from_string(const std::string& s) {
    if (s == "running") return JobStatus::RUNNING;
    if (s == "completed") return JobStatus::COMPLETED;
    if (s == "failed") return JobStatus::FAILED;
    if (s == "cancelled") return JobStatus::CANCELLED;
    return JobStatus::PENDING;
}

WakePolicy wake_policy_from_string(const std::string& s) {
    if (s == "wake_required" || s == "1") return WakePolicy::WAKE_REQUIRED;
    return WakePolicy::NO_WAKE;
}

SleepPolicy sleep_policy_from_string(const std::string& s) {
    if (s == "inhibit" || s == "1") return SleepPolicy::INHIBIT;
    return SleepPolicy::NORMAL;
}

JobAuthority job_authority_from_string(const std::string& s) {
    if (s == "vehicle" || s == "1") return JobAuthority::VEHICLE;
    return JobAuthority::CLOUD;
}

}  // namespace ifex::scheduler
