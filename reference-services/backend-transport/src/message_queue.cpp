#include "message_queue.hpp"

#include <glog/logging.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace ifex::reference {

// =============================================================================
// ContentQueue Implementation
// =============================================================================

ContentQueue::ContentQueue(const Config& config) : config_(config) {}

ContentQueue::~ContentQueue() = default;

uint64_t ContentQueue::Enqueue(std::vector<uint8_t> payload, Persistence persistence) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if queue is full
    if (queue_.size() >= config_.max_buffer_size) {
        // If persistence is BestEffort, we can drop the message
        if (persistence == Persistence::BestEffort) {
            messages_dropped_++;
            VLOG(1) << "Queue " << config_.content_id << " full, dropping best-effort message";
            return 0;
        }

        // For persistent messages, we need to drop the oldest BestEffort message
        // or reject if all messages are persistent
        auto it = std::find_if(queue_.begin(), queue_.end(), [](const auto& msg) {
            return msg->persistence == Persistence::BestEffort;
        });

        if (it != queue_.end()) {
            queue_.erase(it);
            messages_dropped_++;
            VLOG(1) << "Queue " << config_.content_id << " full, dropped oldest best-effort to make room";
        } else {
            // All messages are persistent, reject new message
            LOG(WARNING) << "Queue " << config_.content_id << " full with all persistent messages, rejecting";
            return 0;
        }
    }

    // Assign sequence atomically (sequences start at 1, 0 means error)
    uint64_t sequence = ++next_sequence_;

    auto msg = std::make_unique<QueuedMessage>();
    msg->sequence = sequence;
    msg->content_id = config_.content_id;
    msg->payload = std::move(payload);
    msg->persistence = persistence;
    msg->enqueue_time = std::chrono::steady_clock::now();

    queue_.push_back(std::move(msg));

    VLOG(2) << "Enqueued message to queue " << config_.content_id
            << " (size=" << queue_.size() << ", seq=" << sequence << ")";

    return sequence;
}

std::unique_ptr<QueuedMessage> ContentQueue::TryDequeue() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Only one message in-flight at a time (ordering guarantee)
    if (in_flight_) {
        return nullptr;
    }

    if (queue_.empty()) {
        return nullptr;
    }

    in_flight_ = std::move(queue_.front());
    queue_.pop_front();

    VLOG(2) << "Dequeued message from queue " << config_.content_id
            << " (seq=" << in_flight_->sequence << ", remaining=" << queue_.size() << ")";

    // Return a copy for the caller, keep original as in_flight_
    auto result = std::make_unique<QueuedMessage>(*in_flight_);
    return result;
}

void ContentQueue::AckInFlight() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (in_flight_) {
        VLOG(2) << "Acked message from queue " << config_.content_id
                << " (seq=" << in_flight_->sequence << ")";
        in_flight_.reset();
    }
}

void ContentQueue::NackInFlight() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (in_flight_) {
        in_flight_->retry_count++;

        // Requeue at front to maintain ordering
        VLOG(1) << "Nacked message from queue " << config_.content_id
                << " (seq=" << in_flight_->sequence << ", retry=" << in_flight_->retry_count << ")";

        queue_.push_front(std::move(in_flight_));
        in_flight_.reset();
    }
}

size_t ContentQueue::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() + (in_flight_ ? 1 : 0);
}

bool ContentQueue::IsEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty() && !in_flight_;
}

bool ContentQueue::IsHighWatermark() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() >= config_.high_watermark;
}

bool ContentQueue::IsFull() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() >= config_.max_buffer_size;
}

QueueLevel ContentQueue::GetLevel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty() && !in_flight_) {
        return QueueLevel::Empty;
    }

    size_t size = queue_.size() + (in_flight_ ? 1 : 0);
    size_t capacity = config_.max_buffer_size;
    double pct = static_cast<double>(size) / capacity * 100.0;

    if (pct > 95.0) return QueueLevel::Full;
    if (pct > 75.0) return QueueLevel::Critical;
    if (pct > 50.0) return QueueLevel::High;
    if (pct > 25.0) return QueueLevel::Normal;
    return QueueLevel::Low;
}

std::vector<uint64_t> ContentQueue::PruneStale() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint64_t> pruned;

    auto now = std::chrono::steady_clock::now();

    // Prune stale BestEffort messages from queue
    auto it = queue_.begin();
    while (it != queue_.end()) {
        auto& msg = *it;
        if (msg->persistence == Persistence::BestEffort) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - msg->enqueue_time);
            if (age >= config_.best_effort_ttl) {
                pruned.push_back(msg->sequence);
                VLOG(1) << "Pruning stale BestEffort message seq=" << msg->sequence
                        << " (age=" << age.count() << "s)";
                it = queue_.erase(it);
                continue;
            }
        }
        ++it;
    }

    return pruned;
}

size_t ContentQueue::PersistToDisk(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Only persist messages with Durable persistence
    std::vector<QueuedMessage*> to_persist;

    if (in_flight_ && in_flight_->persistence == Persistence::Durable) {
        to_persist.push_back(in_flight_.get());
    }

    for (const auto& msg : queue_) {
        if (msg->persistence == Persistence::Durable) {
            to_persist.push_back(msg.get());
        }
    }

    if (to_persist.empty()) {
        return 0;
    }

    // Create directory if needed
    std::filesystem::create_directories(path);

    std::string filepath = path + "/queue_" + std::to_string(config_.content_id) + ".bin";
    std::ofstream file(filepath, std::ios::binary);
    if (!file) {
        LOG(ERROR) << "Failed to open " << filepath << " for writing";
        return 0;
    }

    // Simple binary format: count, then for each message: payload_size, payload, persistence
    uint32_t count = static_cast<uint32_t>(to_persist.size());
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto* msg : to_persist) {
        uint32_t payload_size = static_cast<uint32_t>(msg->payload.size());
        file.write(reinterpret_cast<const char*>(&payload_size), sizeof(payload_size));
        file.write(reinterpret_cast<const char*>(msg->payload.data()), payload_size);
        file.write(reinterpret_cast<const char*>(&msg->persistence), sizeof(msg->persistence));
    }

    LOG(INFO) << "Persisted " << count << " messages from queue " << config_.content_id;
    return count;
}

size_t ContentQueue::LoadFromDisk(const std::string& path) {
    std::string filepath = path + "/queue_" + std::to_string(config_.content_id) + ".bin";

    if (!std::filesystem::exists(filepath)) {
        return 0;
    }

    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        LOG(ERROR) << "Failed to open " << filepath << " for reading";
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t count = 0;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t payload_size = 0;
        file.read(reinterpret_cast<char*>(&payload_size), sizeof(payload_size));

        std::vector<uint8_t> payload(payload_size);
        file.read(reinterpret_cast<char*>(payload.data()), payload_size);

        Persistence persistence;
        file.read(reinterpret_cast<char*>(&persistence), sizeof(persistence));

        auto msg = std::make_unique<QueuedMessage>();
        msg->sequence = next_sequence_++;
        msg->content_id = config_.content_id;
        msg->payload = std::move(payload);
        msg->persistence = persistence;
        msg->enqueue_time = std::chrono::steady_clock::now();

        queue_.push_back(std::move(msg));
    }

    // Remove the file after loading
    std::filesystem::remove(filepath);

    LOG(INFO) << "Loaded " << count << " messages into queue " << config_.content_id;
    return count;
}

// =============================================================================
// MessageQueueManager Implementation
// =============================================================================

MessageQueueManager::MessageQueueManager(const Config& config) : config_(config) {}

MessageQueueManager::~MessageQueueManager() {
    Stop();
}

void MessageQueueManager::Start(SendCallback send_cb, ConnectionCallback conn_cb, AckCallback ack_cb) {
    send_cb_ = std::move(send_cb);
    conn_cb_ = std::move(conn_cb);
    ack_cb_ = std::move(ack_cb);

    // Load any persisted messages
    LoadAll();

    running_ = true;
    sender_thread_ = std::thread(&MessageQueueManager::SenderThread, this);

    LOG(INFO) << "MessageQueueManager started";
}

void MessageQueueManager::Stop() {
    if (!running_) return;

    LOG(INFO) << "MessageQueueManager stopping...";

    running_ = false;
    sender_cv_.notify_all();

    if (sender_thread_.joinable()) {
        sender_thread_.join();
    }

    // Persist remaining messages
    PersistAll();

    LOG(INFO) << "MessageQueueManager stopped";
}

uint64_t MessageQueueManager::Enqueue(uint32_t content_id, std::vector<uint8_t> payload, Persistence persistence) {
    auto& queue = GetQueue(content_id);
    uint64_t sequence = queue.Enqueue(std::move(payload), persistence);

    // Wake up sender thread
    if (sequence > 0) {
        sender_cv_.notify_one();
    }

    return sequence;
}

uint32_t MessageQueueManager::GetPendingCount(uint32_t content_id) const {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = queues_.find(content_id);
    if (it != queues_.end()) {
        return static_cast<uint32_t>(it->second->Size());
    }
    return 0;
}

ContentQueue& MessageQueueManager::GetQueue(uint32_t content_id) {
    std::lock_guard<std::mutex> lock(queues_mutex_);

    auto it = queues_.find(content_id);
    if (it == queues_.end()) {
        ContentQueue::Config config;
        config.content_id = content_id;
        config.max_buffer_size = config_.default_queue_size;
        config.high_watermark = config_.default_queue_size * 80 / 100;
        config.low_watermark = config_.default_queue_size * 20 / 100;

        auto queue = std::make_unique<ContentQueue>(config);
        auto* ptr = queue.get();
        queues_[content_id] = std::move(queue);
        queue_order_.push_back(content_id);

        LOG(INFO) << "Created queue for content_id " << content_id;
        return *ptr;
    }

    return *it->second;
}

MessageQueueManager::Stats MessageQueueManager::GetStats() const {
    std::lock_guard<std::mutex> lock(queues_mutex_);

    Stats stats;
    for (const auto& [id, queue] : queues_) {
        stats.total_queued += queue->Size();
        stats.total_capacity += config_.default_queue_size;
    }

    stats.level = GetLevelLocked();
    return stats;
}

QueueLevel MessageQueueManager::GetLevel() const {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    return GetLevelLocked();
}

QueueLevel MessageQueueManager::GetLevelLocked() const {
    // Assumes queues_mutex_ is already held by caller

    if (queues_.empty()) {
        return QueueLevel::Empty;
    }

    // Use worst (highest) level across all queues
    QueueLevel worst = QueueLevel::Empty;
    for (const auto& [id, queue] : queues_) {
        QueueLevel level = queue->GetLevel();
        if (static_cast<uint8_t>(level) > static_cast<uint8_t>(worst)) {
            worst = level;
        }
    }
    return worst;
}

std::unordered_map<uint32_t, std::vector<uint64_t>> MessageQueueManager::PruneAllStale() {
    std::lock_guard<std::mutex> lock(queues_mutex_);

    std::unordered_map<uint32_t, std::vector<uint64_t>> result;
    for (auto& [id, queue] : queues_) {
        auto pruned = queue->PruneStale();
        if (!pruned.empty()) {
            result[id] = std::move(pruned);
        }
    }
    return result;
}

void MessageQueueManager::PersistAll() {
    std::lock_guard<std::mutex> lock(queues_mutex_);

    size_t total = 0;
    for (auto& [id, queue] : queues_) {
        total += queue->PersistToDisk(config_.persistence_dir);
    }

    if (total > 0) {
        LOG(INFO) << "Persisted " << total << " messages total";
    }
}

void MessageQueueManager::LoadAll() {
    // Scan persistence directory for queue files
    if (!std::filesystem::exists(config_.persistence_dir)) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(config_.persistence_dir)) {
        if (entry.path().extension() == ".bin") {
            std::string filename = entry.path().stem().string();
            if (filename.rfind("queue_", 0) == 0) {
                uint32_t content_id = std::stoul(filename.substr(6));
                auto& queue = GetQueue(content_id);
                queue.LoadFromDisk(config_.persistence_dir);
            }
        }
    }
}

void MessageQueueManager::SenderThread() {
    LOG(INFO) << "Sender thread started";

    while (running_) {
        // Wait for work or shutdown
        {
            std::unique_lock<std::mutex> lock(sender_mutex_);
            sender_cv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return !running_ || !IsEmpty();
            });
        }

        if (!running_) break;

        // Check connection
        if (!conn_cb_ || !conn_cb_()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Round-robin through queues
        std::unique_ptr<QueuedMessage> msg;
        ContentQueue* source_queue = nullptr;

        {
            std::lock_guard<std::mutex> lock(queues_mutex_);

            if (queue_order_.empty()) continue;

            // Try each queue in round-robin order
            for (size_t i = 0; i < queue_order_.size(); ++i) {
                size_t idx = (current_queue_idx_ + i) % queue_order_.size();
                uint32_t content_id = queue_order_[idx];

                auto it = queues_.find(content_id);
                if (it != queues_.end()) {
                    msg = it->second->TryDequeue();
                    if (msg) {
                        source_queue = it->second.get();
                        current_queue_idx_ = (idx + 1) % queue_order_.size();
                        break;
                    }
                }
            }
        }

        if (!msg || !source_queue) continue;

        // Save info for ack callback before sending
        uint32_t content_id = msg->content_id;
        uint64_t sequence = msg->sequence;

        // Try to send
        bool success = send_cb_ && send_cb_(content_id, msg->payload);

        if (success) {
            source_queue->AckInFlight();
            // Notify listeners of successful delivery
            if (ack_cb_) {
                ack_cb_(content_id, sequence);
            }
        } else {
            // Check retry count
            if (msg->retry_count >= config_.max_retries) {
                LOG(WARNING) << "Message exceeded max retries, dropping (content_id="
                             << content_id << ")";
                source_queue->AckInFlight();  // Remove from queue
            } else {
                source_queue->NackInFlight();
                std::this_thread::sleep_for(config_.send_retry_delay);
            }
        }
    }

    LOG(INFO) << "Sender thread stopped";
}

bool MessageQueueManager::IsEmpty() const {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    for (const auto& [id, queue] : queues_) {
        if (!queue->IsEmpty()) return false;
    }
    return true;
}

}  // namespace ifex::reference
