#pragma once

// Time Conversion Utilities for Cloud Scheduler Service
//
// All scheduler APIs use epoch milliseconds (uint64) as the canonical time format.
// These convenience functions convert to/from ISO8601 strings for human readability.
//
// Usage in C++:
//   uint64_t ms = TimeUtils::Iso8601ToEpochMs("2024-12-25T10:30:00Z");
//   std::string iso = TimeUtils::EpochMsToIso8601(ms);
//
// For Python clients (fleet_api.py):
//   from datetime import datetime, timezone
//
//   def iso8601_to_epoch_ms(iso_str: str) -> int:
//       dt = datetime.fromisoformat(iso_str.replace('Z', '+00:00'))
//       return int(dt.timestamp() * 1000)
//
//   def epoch_ms_to_iso8601(epoch_ms: int) -> str:
//       dt = datetime.fromtimestamp(epoch_ms / 1000, tz=timezone.utc)
//       return dt.strftime('%Y-%m-%dT%H:%M:%S.') + f'{(epoch_ms % 1000):03d}Z'
//
// For JavaScript clients (dashboard):
//   function iso8601ToEpochMs(isoStr) {
//       return new Date(isoStr).getTime();
//   }
//
//   function epochMsToIso8601(epochMs) {
//       return new Date(epochMs).toISOString();
//   }

#include <string>
#include <cstdint>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace ifex::cloud::scheduler {

class TimeUtils {
public:
    // Convert ISO8601 datetime string to epoch milliseconds
    // Supports formats: "2024-12-25T10:30:00Z", "2024-12-25T10:30:00.123Z"
    // Returns 0 for empty or invalid input
    static uint64_t Iso8601ToEpochMs(const std::string& iso_str) {
        if (iso_str.empty()) return 0;

        std::tm tm = {};
        std::istringstream ss(iso_str);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
        if (ss.fail()) return 0;

        time_t epoch_sec = timegm(&tm);

        // Parse optional milliseconds (.123)
        uint64_t ms = 0;
        char c;
        if (ss >> c && c == '.') {
            int frac;
            if (ss >> frac) {
                std::string frac_str = std::to_string(frac);
                while (frac_str.length() < 3) frac_str += "0";
                ms = std::stoull(frac_str.substr(0, 3));
            }
        }

        return static_cast<uint64_t>(epoch_sec) * 1000 + ms;
    }

    // Convert epoch milliseconds to ISO8601 datetime string
    // Returns empty string for 0 input
    // Output format: "2024-12-25T10:30:00.123Z"
    static std::string EpochMsToIso8601(uint64_t epoch_ms) {
        if (epoch_ms == 0) return "";

        auto seconds = static_cast<time_t>(epoch_ms / 1000);
        auto ms = epoch_ms % 1000;

        std::tm tm;
        gmtime_r(&seconds, &tm);

        std::ostringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3) << ms << 'Z';
        return ss.str();
    }

    // Get current time as epoch milliseconds
    static uint64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Convert std::chrono::system_clock::time_point to epoch milliseconds
    static uint64_t TimePointToMs(const std::chrono::system_clock::time_point& tp) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            tp.time_since_epoch()).count();
    }

    // Convert epoch milliseconds to std::chrono::system_clock::time_point
    static std::chrono::system_clock::time_point MsToTimePoint(uint64_t ms) {
        return std::chrono::system_clock::time_point(
            std::chrono::milliseconds(ms));
    }
};

} // namespace ifex::cloud::scheduler
