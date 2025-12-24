#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ifex::reference {

/// Persistence level for messages (internal representation)
/// Maps to proto persistence_t at gRPC boundary.
/// All levels preserve ordering - messages are always queued for FIFO.
enum class Persistence : uint8_t {
    BestEffort = 0,  ///< Queued for ordering, pruned if stale. No retry on failure.
    Volatile = 1,    ///< Retry until delivered. Memory queue, lost on any shutdown.
    Durable = 2      ///< Retry until delivered. Persisted on graceful shutdown only.
};

/// Queue fill level for adaptive throttling (internal representation)
/// Maps to proto queue_level_t at gRPC boundary.
enum class QueueLevel : uint8_t {
    Empty = 0,     ///< 0%
    Low = 1,       ///< < 25%
    Normal = 2,    ///< 25-50%
    High = 3,      ///< 50-75% - consider throttling
    Critical = 4,  ///< 75-95% - throttle low-priority
    Full = 5       ///< > 95%
};

/// Message waiting to be sent
struct QueuedMessage {
    uint64_t sequence;              ///< Per-queue sequence number for ordering
    uint32_t content_id;
    std::vector<uint8_t> payload;
    Persistence persistence;
    std::chrono::steady_clock::time_point enqueue_time;
    int retry_count = 0;

    // For persistent messages
    bool persisted_to_disk = false;
};

/// Per-content-id message queue with ordering guarantees
///
/// Guarantees:
/// - Messages are dequeued in FIFO order (preserves send order)
/// - Only one message per content_id in-flight at a time (ordering to broker)
/// - Configurable buffer size with overflow handling based on persistence
class ContentQueue {
public:
    struct Config {
        uint32_t content_id;
        size_t max_buffer_size = 1000;      ///< Max messages in buffer
        size_t high_watermark = 800;        ///< Start applying backpressure
        size_t low_watermark = 200;         ///< Resume normal operation
        std::chrono::seconds best_effort_ttl{30};  ///< TTL for BestEffort messages
    };

    explicit ContentQueue(const Config& config);
    ~ContentQueue();

    /// Enqueue a message, assigns sequence atomically
    /// @return assigned sequence if enqueued, 0 if dropped (queue full + low persistence)
    uint64_t Enqueue(std::vector<uint8_t> payload, Persistence persistence);

    /// Try to get next message to send (non-blocking)
    /// @return message if available, nullptr otherwise
    std::unique_ptr<QueuedMessage> TryDequeue();

    /// Mark current in-flight message as sent successfully
    void AckInFlight();

    /// Mark current in-flight message as failed, requeue for retry
    void NackInFlight();

    /// Get queue statistics
    size_t Size() const;
    bool IsEmpty() const;
    bool IsHighWatermark() const;
    bool IsFull() const;
    QueueLevel GetLevel() const;

    /// Prune stale BestEffort messages (called during disconnect)
    /// @return sequences of pruned messages (for gap detection)
    std::vector<uint64_t> PruneStale();

    /// Persist all messages to disk (for graceful shutdown)
    /// @return number of messages persisted
    size_t PersistToDisk(const std::string& path);

    /// Load persisted messages from disk
    /// @return number of messages loaded
    size_t LoadFromDisk(const std::string& path);

    uint32_t content_id() const { return config_.content_id; }

private:
    Config config_;
    mutable std::mutex mutex_;

    std::deque<std::unique_ptr<QueuedMessage>> queue_;
    std::unique_ptr<QueuedMessage> in_flight_;  ///< Currently being sent

    uint64_t next_sequence_ = 0;
    uint64_t messages_dropped_ = 0;
};

/// Manages multiple content queues with a shared sender thread
///
/// Design:
/// - One queue per content_id (created on demand)
/// - Single sender thread ensures ordering across queues
/// - Round-robin among non-empty queues for fairness
class MessageQueueManager {
public:
    using SendCallback = std::function<bool(uint32_t content_id, const std::vector<uint8_t>& payload)>;
    using ConnectionCallback = std::function<bool()>;  // Returns true if connected

    struct Config {
        size_t default_queue_size = 1000;
        std::string persistence_dir = "/var/lib/ifex/backend-transport";
        std::chrono::milliseconds send_retry_delay{100};
        int max_retries = 3;
    };

    explicit MessageQueueManager(const Config& config);
    ~MessageQueueManager();

    /// Start the sender thread
    void Start(SendCallback send_cb, ConnectionCallback conn_cb);

    /// Stop the sender thread and persist messages
    void Stop();

    /// Enqueue a message for sending, assigns sequence atomically
    /// @return assigned sequence if enqueued, 0 if dropped
    uint64_t Enqueue(uint32_t content_id, std::vector<uint8_t> payload, Persistence persistence);

    /// Get or create queue for content_id
    ContentQueue& GetQueue(uint32_t content_id);

    /// Get pending count for a specific content_id
    uint32_t GetPendingCount(uint32_t content_id) const;

    /// Get aggregate statistics
    struct Stats {
        size_t total_queued = 0;
        size_t total_in_flight = 0;
        size_t total_capacity = 0;
        QueueLevel level = QueueLevel::Empty;
    };
    Stats GetStats() const;

    /// Get aggregate queue level across all queues
    QueueLevel GetLevel() const;

    /// Prune stale BestEffort messages from all queues
    /// @return map of content_id -> pruned sequences
    std::unordered_map<uint32_t, std::vector<uint64_t>> PruneAllStale();

    /// Check if all queues are empty
    bool IsEmpty() const;

    /// Persist all queues to disk
    void PersistAll();

    /// Load all persisted queues from disk
    void LoadAll();

private:
    void SenderThread();

    Config config_;
    SendCallback send_cb_;
    ConnectionCallback conn_cb_;

    mutable std::mutex queues_mutex_;
    std::unordered_map<uint32_t, std::unique_ptr<ContentQueue>> queues_;

    std::atomic<bool> running_{false};
    std::thread sender_thread_;
    std::condition_variable sender_cv_;
    std::mutex sender_mutex_;

    // Round-robin state
    std::vector<uint32_t> queue_order_;
    size_t current_queue_idx_ = 0;
};

}  // namespace ifex::reference
