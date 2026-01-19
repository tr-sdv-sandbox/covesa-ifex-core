// Version Vector for Scheduler Sync Protocol v2
//
// Two-component version vector for conflict detection between cloud and vehicle.
// Each component is incremented by its respective side on any change.
// No dependency on wall-clock time for correctness.

#pragma once

#include <cstdint>
#include <string>
#include <algorithm>

namespace ifex::scheduler {

// Result of comparing two version vectors
enum class CompareResult {
    EQUAL,             // Versions are identical
    LOCAL_DOMINATES,   // Local has all changes remote has, plus more
    REMOTE_DOMINATES,  // Remote has all changes local has, plus more
    CONFLICT           // Neither dominates - concurrent modifications
};

// Two-component version vector for conflict detection.
// Cloud increments cloud_seq, vehicle increments vehicle_seq.
struct VersionVector {
    uint64_t cloud_seq = 0;   // Incremented by cloud on any change
    uint64_t vehicle_seq = 0; // Incremented by vehicle on any change

    // Default constructor
    VersionVector() = default;

    // Constructor with values
    VersionVector(uint64_t cloud, uint64_t vehicle)
        : cloud_seq(cloud), vehicle_seq(vehicle) {}

    // Check if this version dominates another.
    // A dominates B iff:
    //   A.cloud >= B.cloud AND A.vehicle >= B.vehicle
    //   AND at least one is strictly greater
    bool dominates(const VersionVector& other) const {
        return (cloud_seq >= other.cloud_seq &&
                vehicle_seq >= other.vehicle_seq &&
                (cloud_seq > other.cloud_seq || vehicle_seq > other.vehicle_seq));
    }

    // Check if versions are equal
    bool equals(const VersionVector& other) const {
        return cloud_seq == other.cloud_seq && vehicle_seq == other.vehicle_seq;
    }

    // Merge two version vectors (component-wise maximum).
    // Used after conflict resolution to create a version that
    // dominates both inputs.
    static VersionVector merge(const VersionVector& a, const VersionVector& b) {
        return VersionVector(
            std::max(a.cloud_seq, b.cloud_seq),
            std::max(a.vehicle_seq, b.vehicle_seq));
    }

    // Increment cloud sequence (call this on cloud side before any change)
    void increment_cloud() { ++cloud_seq; }

    // Increment vehicle sequence (call this on vehicle side before any change)
    void increment_vehicle() { ++vehicle_seq; }

    // String representation for logging
    std::string to_string() const {
        return "{cloud:" + std::to_string(cloud_seq) +
               ", vehicle:" + std::to_string(vehicle_seq) + "}";
    }

    // Equality operators
    bool operator==(const VersionVector& other) const { return equals(other); }
    bool operator!=(const VersionVector& other) const { return !equals(other); }
};

// Compare two version vectors and determine relationship.
// This is the core logic for deciding what to do when receiving a remote update.
inline CompareResult compare(const VersionVector& local, const VersionVector& remote) {
    if (local.equals(remote)) {
        return CompareResult::EQUAL;
    }
    if (local.dominates(remote)) {
        return CompareResult::LOCAL_DOMINATES;
    }
    if (remote.dominates(local)) {
        return CompareResult::REMOTE_DOMINATES;
    }
    return CompareResult::CONFLICT;
}

// String representation of CompareResult
inline const char* compare_result_to_string(CompareResult result) {
    switch (result) {
        case CompareResult::EQUAL: return "EQUAL";
        case CompareResult::LOCAL_DOMINATES: return "LOCAL_DOMINATES";
        case CompareResult::REMOTE_DOMINATES: return "REMOTE_DOMINATES";
        case CompareResult::CONFLICT: return "CONFLICT";
        default: return "UNKNOWN";
    }
}

}  // namespace ifex::scheduler

// Backwards compatibility alias
namespace ifex::sync {
    using VersionVector = ifex::scheduler::VersionVector;
    using CompareResult = ifex::scheduler::CompareResult;
    using ifex::scheduler::compare;
    using ifex::scheduler::compare_result_to_string;
}
