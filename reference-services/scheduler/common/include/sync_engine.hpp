// Sync Engine for Scheduler Sync Protocol v2
//
// Implements the core sync logic for bidirectional job synchronization
// between cloud and vehicle. Uses version vectors for conflict detection
// and source authority for deterministic conflict resolution.
//
// See docs/scheduler-sync-protocol-v2.md for full specification.

#pragma once

#include "version_vector.hpp"
#include "job.hpp"
#include <optional>
#include <string>

namespace ifex::scheduler {

// Result of processing a sync operation
struct SyncResult {
    enum Action {
        NO_ACTION,          // No change needed (already in sync)
        ACCEPT_REMOTE,      // Accept remote version (remote dominates)
        REJECT_REMOTE,      // Reject remote version (local dominates)
        CONFLICT_RESOLVED   // Conflict resolved using authority
    };

    Action action = NO_ACTION;
    VersionVector resolved_version;  // Version after resolution
    std::string winner;              // "cloud" or "vehicle" (for conflict)
    std::string reason;              // Why this action was taken

    static SyncResult no_action() {
        return {NO_ACTION, {}, "", "versions equal"};
    }

    static SyncResult accept(const VersionVector& version) {
        return {ACCEPT_REMOTE, version, "", "remote dominates"};
    }

    static SyncResult reject(const VersionVector& version) {
        return {REJECT_REMOTE, version, "", "local dominates"};
    }

    static SyncResult conflict_resolved(
            const VersionVector& merged_version,
            const std::string& winner,
            const std::string& reason) {
        return {CONFLICT_RESOLVED, merged_version, winner, reason};
    }
};

// Core sync engine - stateless, pure functions
class SyncEngine {
public:
    // Process an incoming job from remote side.
    //
    // Parameters:
    //   remote_version - Version vector of the incoming job
    //   local_version  - Version vector of local job (nullopt if new)
    //   authority      - Who created this job (determines conflict winner)
    //   is_cloud_side  - Are we running on cloud (true) or vehicle (false)?
    //
    // Returns:
    //   SyncResult indicating what action to take
    static SyncResult process_remote(
            const VersionVector& remote_version,
            const std::optional<VersionVector>& local_version,
            JobAuthority authority,
            bool is_cloud_side) {
        // New job - accept it
        if (!local_version.has_value()) {
            return SyncResult::accept(remote_version);
        }

        const VersionVector& local = local_version.value();
        CompareResult cmp = compare(local, remote_version);

        switch (cmp) {
            case CompareResult::EQUAL:
                return SyncResult::no_action();

            case CompareResult::REMOTE_DOMINATES:
                return SyncResult::accept(remote_version);

            case CompareResult::LOCAL_DOMINATES:
                return SyncResult::reject(local);

            case CompareResult::CONFLICT:
                return resolve_conflict(local, remote_version, authority, is_cloud_side);
        }

        return SyncResult::no_action();  // Should never reach here
    }

    // Resolve a conflict using source authority.
    //
    // Rules:
    //   - AUTHORITY_CLOUD: cloud's version wins
    //   - AUTHORITY_VEHICLE: vehicle's version wins
    //
    // The resolved version is the component-wise maximum (merge),
    // so both sides end up at a version that dominates both inputs.
    static SyncResult resolve_conflict(
            const VersionVector& local_version,
            const VersionVector& remote_version,
            JobAuthority authority,
            bool is_cloud_side) {
        // Merge versions: component-wise maximum
        VersionVector merged = VersionVector::merge(local_version, remote_version);

        // Determine winner based on authority
        bool cloud_wins = (authority == JobAuthority::CLOUD);
        bool we_are_cloud = is_cloud_side;

        // Did we win?
        bool we_win = (cloud_wins == we_are_cloud);

        std::string winner = cloud_wins ? "cloud" : "vehicle";
        std::string reason = "authority=" + std::string(job_authority_to_string(authority));

        // After conflict resolution, increment our sequence to indicate
        // we processed the conflict
        if (we_are_cloud) {
            merged.increment_cloud();
        } else {
            merged.increment_vehicle();
        }

        return SyncResult::conflict_resolved(merged, winner, reason);
    }

    // Prepare a version for sending to remote.
    // Call this before modifying a job locally.
    //
    // Parameters:
    //   current_version - Current version of the job
    //   is_cloud_side   - Are we running on cloud (true) or vehicle (false)?
    //
    // Returns:
    //   New version with our sequence incremented
    static VersionVector prepare_for_local_change(
            const VersionVector& current_version,
            bool is_cloud_side) {
        VersionVector new_version = current_version;
        if (is_cloud_side) {
            new_version.increment_cloud();
        } else {
            new_version.increment_vehicle();
        }
        return new_version;
    }

    // Check if a job should be accepted as a new job.
    // New jobs must have proper authority set.
    static bool validate_new_job(JobAuthority authority, bool is_cloud_side) {
        // Cloud can only create AUTHORITY_CLOUD jobs
        // Vehicle can only create AUTHORITY_VEHICLE jobs
        if (is_cloud_side) {
            return authority == JobAuthority::CLOUD;
        } else {
            return authority == JobAuthority::VEHICLE;
        }
    }

    // Generate a job ID with proper namespace prefix.
    //
    // Namespacing prevents ID collisions:
    //   - cloud-* : Jobs created by cloud
    //   - veh-VIN-* : Jobs created by vehicle
    //   - phone-* : Jobs created by companion app
    static std::string generate_job_id(
            const std::string& base_id,
            JobAuthority authority,
            const std::string& vehicle_id = "") {
        switch (authority) {
            case JobAuthority::CLOUD:
                return "cloud-" + base_id;
            case JobAuthority::VEHICLE:
                if (!vehicle_id.empty()) {
                    return "veh-" + vehicle_id + "-" + base_id;
                }
                return "veh-" + base_id;
            default:
                return base_id;
        }
    }
};

}  // namespace ifex::scheduler

// Backwards compatibility alias
namespace ifex::sync {
    using JobAuthority = ifex::scheduler::JobAuthority;
    using SyncResult = ifex::scheduler::SyncResult;
    using SyncEngine = ifex::scheduler::SyncEngine;

    inline const char* authority_to_string(JobAuthority auth) {
        return ifex::scheduler::job_authority_to_string(auth);
    }
}
