#include "../include/sqlite_cloud_vehicle_db_adapter.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ifex {
namespace sync {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

class Statement {
public:
    Statement(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    ~Statement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }

    sqlite3_stmt* get() const {
        return stmt_;
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) {
        execute(db_, "BEGIN IMMEDIATE TRANSACTION");
    }

    ~Transaction() {
        if (!committed_) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    void commit() {
        execute(db_, "COMMIT");
        committed_ = true;
    }

private:
    static void execute(sqlite3* db, const char* sql) {
        if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
    }

    sqlite3* db_ = nullptr;
    bool committed_ = false;
};

struct StoredLedgerEntry {
    bool found = false;
    ApplyDisposition disposition = ApplyDisposition::kApplied;
    VersionVector durable_version;
    bool has_persisted_conflict = false;
    std::int64_t conflict_id = 0;
};

struct StoredConflict {
    std::int64_t id = 0;
    ConflictRecord conflict;
};

std::string db_error(sqlite3* db, const std::string& context) {
    return context + ": " + sqlite3_errmsg(db);
}

void execute(sqlite3* db, const std::string& sql) {
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error(db_error(db, sql));
    }
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    if (sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error("failed to bind text");
    }
}

void bind_blob(sqlite3_stmt* stmt, int index, const ByteBuffer& value) {
    const int rc = value.empty()
                       ? sqlite3_bind_zeroblob(stmt, index, 0)
                       : sqlite3_bind_blob(stmt,
                                           index,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("failed to bind blob");
    }
}

void bind_u64(sqlite3_stmt* stmt, int index, std::uint64_t value) {
    if (sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
        throw std::runtime_error("failed to bind integer");
    }
}

void bind_i32(sqlite3_stmt* stmt, int index, int value) {
    if (sqlite3_bind_int(stmt, index, value) != SQLITE_OK) {
        throw std::runtime_error("failed to bind integer");
    }
}

bool step_row(sqlite3* db, sqlite3_stmt* stmt) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    throw std::runtime_error(db_error(db, "sqlite3_step"));
}

void step_done(sqlite3* db, sqlite3_stmt* stmt) {
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw std::runtime_error(db_error(db, "sqlite3_step"));
    }
}

std::string column_text(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(text));
}

ByteBuffer column_blob(sqlite3_stmt* stmt, int index) {
    const auto* blob = static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt, index));
    const int size = sqlite3_column_bytes(stmt, index);
    if (blob == nullptr || size <= 0) {
        return {};
    }
    return ByteBuffer(blob, blob + size);
}

std::uint64_t column_u64(sqlite3_stmt* stmt, int index) {
    return static_cast<std::uint64_t>(sqlite3_column_int64(stmt, index));
}

bool column_bool(sqlite3_stmt* stmt, int index) {
    return sqlite3_column_int(stmt, index) != 0;
}

bool has_locator(const RecordLocator& locator) {
    return !locator.namespace_name.empty() || !locator.origin_node_id.empty() || !locator.record_id.empty();
}

bool has_session_limit(std::size_t limit) {
    return limit != 0;
}

void mix_bytes(std::uint64_t& hash, const ByteBuffer& value) {
    for (std::uint8_t byte : value) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kFnvPrime;
    }
}

void mix_string(std::uint64_t& hash, const std::string& value) {
    mix_bytes(hash, ByteBuffer(value.begin(), value.end()));
}

void mix_u64(std::uint64_t& hash, std::uint64_t value) {
    for (int shift = 0; shift < 8; ++shift) {
        hash ^= static_cast<std::uint64_t>((value >> (shift * 8)) & 0xFFU);
        hash *= kFnvPrime;
    }
}

void mix_u32(std::uint64_t& hash, std::uint32_t value) {
    for (int shift = 0; shift < 4; ++shift) {
        hash ^= static_cast<std::uint64_t>((value >> (shift * 8)) & 0xFFU);
        hash *= kFnvPrime;
    }
}

std::string records_select_sql(const std::string& where_clause, bool with_limit) {
    std::ostringstream sql;
    sql << "SELECT namespace_name, origin_node_id, record_id, cloud_seq, truck_seq, operation, "
           "payload, schema_version, idempotency_key, correlation_id, payload_checksum, "
           "wall_clock_ms, created_at_ms, updated_at_ms, tombstone_at_ms, tombstone_reason "
           "FROM canonical_records "
        << where_clause
        << " ORDER BY namespace_name, origin_node_id, record_id";
    if (with_limit) {
        sql << " LIMIT ?";
    }
    return sql.str();
}

CanonicalRecord read_record_row(sqlite3_stmt* stmt) {
    CanonicalRecord record;
    record.locator.namespace_name = column_text(stmt, 0);
    record.locator.origin_node_id = column_text(stmt, 1);
    record.locator.record_id = column_blob(stmt, 2);
    record.version_vector.cloud_seq = column_u64(stmt, 3);
    record.version_vector.truck_seq = column_u64(stmt, 4);
    record.operation = static_cast<RecordOperation>(sqlite3_column_int(stmt, 5));
    record.payload = column_blob(stmt, 6);
    record.schema_version = static_cast<std::uint32_t>(sqlite3_column_int(stmt, 7));
    record.idempotency_key = column_text(stmt, 8);
    record.correlation_id = column_text(stmt, 9);
    record.payload_checksum = column_u64(stmt, 10);
    record.wall_clock_ms = column_u64(stmt, 11);
    record.created_at_ms = column_u64(stmt, 12);
    record.updated_at_ms = column_u64(stmt, 13);
    record.tombstone_at_ms = column_u64(stmt, 14);
    record.tombstone_reason = column_text(stmt, 15);
    return record;
}

std::optional<CanonicalRecord> load_record(sqlite3* db, const RecordLocator& locator) {
    Statement stmt(db,
                   records_select_sql(
                       "WHERE namespace_name = ? AND origin_node_id = ? AND record_id = ?",
                       false));
    bind_text(stmt.get(), 1, locator.namespace_name);
    bind_text(stmt.get(), 2, locator.origin_node_id);
    bind_blob(stmt.get(), 3, locator.record_id);

    if (!step_row(db, stmt.get())) {
        return std::nullopt;
    }
    return read_record_row(stmt.get());
}

void upsert_record(sqlite3* db, CanonicalRecord record, const std::string& idempotency_key) {
    record.idempotency_key = idempotency_key;

    Statement stmt(db,
                   "INSERT INTO canonical_records ("
                   "namespace_name, origin_node_id, record_id, cloud_seq, truck_seq, operation, "
                   "payload, schema_version, idempotency_key, correlation_id, payload_checksum, "
                   "wall_clock_ms, created_at_ms, updated_at_ms, tombstone_at_ms, tombstone_reason) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                   "ON CONFLICT(namespace_name, origin_node_id, record_id) DO UPDATE SET "
                   "cloud_seq = excluded.cloud_seq, "
                   "truck_seq = excluded.truck_seq, "
                   "operation = excluded.operation, "
                   "payload = excluded.payload, "
                   "schema_version = excluded.schema_version, "
                   "idempotency_key = excluded.idempotency_key, "
                   "correlation_id = excluded.correlation_id, "
                   "payload_checksum = excluded.payload_checksum, "
                   "wall_clock_ms = excluded.wall_clock_ms, "
                   "created_at_ms = excluded.created_at_ms, "
                   "updated_at_ms = excluded.updated_at_ms, "
                   "tombstone_at_ms = excluded.tombstone_at_ms, "
                   "tombstone_reason = excluded.tombstone_reason");

    bind_text(stmt.get(), 1, record.locator.namespace_name);
    bind_text(stmt.get(), 2, record.locator.origin_node_id);
    bind_blob(stmt.get(), 3, record.locator.record_id);
    bind_u64(stmt.get(), 4, record.version_vector.cloud_seq);
    bind_u64(stmt.get(), 5, record.version_vector.truck_seq);
    bind_i32(stmt.get(), 6, static_cast<int>(record.operation));
    bind_blob(stmt.get(), 7, record.payload);
    bind_i32(stmt.get(), 8, static_cast<int>(record.schema_version));
    bind_text(stmt.get(), 9, record.idempotency_key);
    bind_text(stmt.get(), 10, record.correlation_id);
    bind_u64(stmt.get(), 11, record.payload_checksum);
    bind_u64(stmt.get(), 12, record.wall_clock_ms);
    bind_u64(stmt.get(), 13, record.created_at_ms);
    bind_u64(stmt.get(), 14, record.updated_at_ms);
    bind_u64(stmt.get(), 15, record.tombstone_at_ms);
    bind_text(stmt.get(), 16, record.tombstone_reason);
    step_done(db, stmt.get());
}

std::int64_t insert_conflict(sqlite3* db, const ConflictRecord& conflict) {
    Statement stmt(db,
                   "INSERT INTO conflicts ("
                   "namespace_name, origin_node_id, record_id, local_cloud_seq, local_truck_seq, "
                   "remote_cloud_seq, remote_truck_seq, local_payload, remote_payload, conflict_class, "
                   "detected_at_ms, correlation_id, resolved) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    bind_text(stmt.get(), 1, conflict.locator.namespace_name);
    bind_text(stmt.get(), 2, conflict.locator.origin_node_id);
    bind_blob(stmt.get(), 3, conflict.locator.record_id);
    bind_u64(stmt.get(), 4, conflict.local_version.cloud_seq);
    bind_u64(stmt.get(), 5, conflict.local_version.truck_seq);
    bind_u64(stmt.get(), 6, conflict.remote_version.cloud_seq);
    bind_u64(stmt.get(), 7, conflict.remote_version.truck_seq);
    bind_blob(stmt.get(), 8, conflict.local_payload);
    bind_blob(stmt.get(), 9, conflict.remote_payload);
    bind_i32(stmt.get(), 10, static_cast<int>(conflict.conflict_class));
    bind_u64(stmt.get(), 11, conflict.detected_at_ms);
    bind_text(stmt.get(), 12, conflict.correlation_id);
    bind_i32(stmt.get(), 13, conflict.resolved ? 1 : 0);
    step_done(db, stmt.get());
    return sqlite3_last_insert_rowid(db);
}

StoredConflict read_conflict_row(sqlite3_stmt* stmt) {
    StoredConflict stored;
    stored.id = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 0));
    stored.conflict.locator.namespace_name = column_text(stmt, 1);
    stored.conflict.locator.origin_node_id = column_text(stmt, 2);
    stored.conflict.locator.record_id = column_blob(stmt, 3);
    stored.conflict.local_version.cloud_seq = column_u64(stmt, 4);
    stored.conflict.local_version.truck_seq = column_u64(stmt, 5);
    stored.conflict.remote_version.cloud_seq = column_u64(stmt, 6);
    stored.conflict.remote_version.truck_seq = column_u64(stmt, 7);
    stored.conflict.local_payload = column_blob(stmt, 8);
    stored.conflict.remote_payload = column_blob(stmt, 9);
    stored.conflict.conflict_class = static_cast<ConflictClass>(sqlite3_column_int(stmt, 10));
    stored.conflict.detected_at_ms = column_u64(stmt, 11);
    stored.conflict.correlation_id = column_text(stmt, 12);
    stored.conflict.resolved = column_bool(stmt, 13);
    return stored;
}

std::optional<ConflictRecord> load_conflict(sqlite3* db, std::int64_t conflict_id) {
    Statement stmt(db,
                   "SELECT id, namespace_name, origin_node_id, record_id, local_cloud_seq, "
                   "local_truck_seq, remote_cloud_seq, remote_truck_seq, local_payload, remote_payload, "
                   "conflict_class, detected_at_ms, correlation_id, resolved "
                   "FROM conflicts WHERE id = ?");
    bind_u64(stmt.get(), 1, static_cast<std::uint64_t>(conflict_id));
    if (!step_row(db, stmt.get())) {
        return std::nullopt;
    }
    return read_conflict_row(stmt.get()).conflict;
}

StoredLedgerEntry load_ledger(sqlite3* db, const std::string& idempotency_key) {
    StoredLedgerEntry entry;
    Statement stmt(db,
                   "SELECT apply_outcome, durable_cloud_seq, durable_truck_seq, has_persisted_conflict, "
                   "conflict_id FROM idempotency_ledger WHERE idempotency_key = ?");
    bind_text(stmt.get(), 1, idempotency_key);
    if (!step_row(db, stmt.get())) {
        return entry;
    }

    entry.found = true;
    entry.disposition = static_cast<ApplyDisposition>(sqlite3_column_int(stmt.get(), 0));
    entry.durable_version.cloud_seq = column_u64(stmt.get(), 1);
    entry.durable_version.truck_seq = column_u64(stmt.get(), 2);
    entry.has_persisted_conflict = column_bool(stmt.get(), 3);
    entry.conflict_id = static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 4));
    return entry;
}

void insert_ledger(sqlite3* db,
                   const std::string& idempotency_key,
                   const RecordLocator& locator,
                   ApplyDisposition disposition,
                   const VersionVector& durable_version,
                   bool has_persisted_conflict,
                   std::int64_t conflict_id) {
    Statement stmt(db,
                   "INSERT OR REPLACE INTO idempotency_ledger ("
                   "idempotency_key, namespace_name, origin_node_id, record_id, durable_cloud_seq, "
                   "durable_truck_seq, apply_outcome, has_persisted_conflict, conflict_id) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    bind_text(stmt.get(), 1, idempotency_key);
    bind_text(stmt.get(), 2, locator.namespace_name);
    bind_text(stmt.get(), 3, locator.origin_node_id);
    bind_blob(stmt.get(), 4, locator.record_id);
    bind_u64(stmt.get(), 5, durable_version.cloud_seq);
    bind_u64(stmt.get(), 6, durable_version.truck_seq);
    bind_i32(stmt.get(), 7, static_cast<int>(disposition));
    bind_i32(stmt.get(), 8, has_persisted_conflict ? 1 : 0);
    if (conflict_id == 0) {
        sqlite3_bind_null(stmt.get(), 9);
    } else {
        bind_u64(stmt.get(), 9, static_cast<std::uint64_t>(conflict_id));
    }
    step_done(db, stmt.get());
}

std::optional<VersionVector> load_remote_ack(sqlite3* db,
                                             const SyncSessionKey& session,
                                             const RecordLocator& locator) {
    Statement stmt(db,
                   "SELECT ack_cloud_seq, ack_truck_seq FROM remote_acks "
                   "WHERE local_node_id = ? AND remote_node_id = ? AND namespace_name = ? "
                   "AND record_namespace = ? AND record_origin_node_id = ? AND record_id = ?");
    bind_text(stmt.get(), 1, session.local_node_id);
    bind_text(stmt.get(), 2, session.remote_node_id);
    bind_text(stmt.get(), 3, session.namespace_name);
    bind_text(stmt.get(), 4, locator.namespace_name);
    bind_text(stmt.get(), 5, locator.origin_node_id);
    bind_blob(stmt.get(), 6, locator.record_id);
    if (!step_row(db, stmt.get())) {
        return std::nullopt;
    }

    VersionVector version;
    version.cloud_seq = column_u64(stmt.get(), 0);
    version.truck_seq = column_u64(stmt.get(), 1);
    return version;
}

void maybe_store_remote_ack(sqlite3* db,
                            const SyncSessionKey& session,
                            const RecordLocator& locator,
                            const VersionVector& version) {
    const std::optional<VersionVector> existing = load_remote_ack(db, session, locator);
    if (existing.has_value()) {
        const CompareResult cmp = compare_versions(*existing, version);
        if (cmp == CompareResult::kEqual || cmp == CompareResult::kLocalDominates) {
            return;
        }
    }

    Statement stmt(db,
                   "INSERT INTO remote_acks ("
                   "local_node_id, remote_node_id, namespace_name, record_namespace, record_origin_node_id, "
                   "record_id, ack_cloud_seq, ack_truck_seq) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                   "ON CONFLICT(local_node_id, remote_node_id, namespace_name, record_namespace, "
                   "record_origin_node_id, record_id) DO UPDATE SET "
                   "ack_cloud_seq = excluded.ack_cloud_seq, ack_truck_seq = excluded.ack_truck_seq");
    bind_text(stmt.get(), 1, session.local_node_id);
    bind_text(stmt.get(), 2, session.remote_node_id);
    bind_text(stmt.get(), 3, session.namespace_name);
    bind_text(stmt.get(), 4, locator.namespace_name);
    bind_text(stmt.get(), 5, locator.origin_node_id);
    bind_blob(stmt.get(), 6, locator.record_id);
    bind_u64(stmt.get(), 7, version.cloud_seq);
    bind_u64(stmt.get(), 8, version.truck_seq);
    step_done(db, stmt.get());
}

void store_checkpoint(sqlite3* db,
                      const SyncSessionKey& session,
                      const CheckpointToken& checkpoint) {
    const std::string last_namespace = checkpoint.last_record.namespace_name.empty()
                                           ? session.namespace_name
                                           : checkpoint.last_record.namespace_name;
    Statement stmt(db,
                   "INSERT INTO checkpoints ("
                   "local_node_id, remote_node_id, namespace_name, sequence_number, last_record_id, "
                   "last_origin_node_id, last_namespace, last_cloud_seq, last_truck_seq) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
                   "ON CONFLICT(local_node_id, remote_node_id, namespace_name) DO UPDATE SET "
                   "sequence_number = excluded.sequence_number, "
                   "last_record_id = excluded.last_record_id, "
                   "last_origin_node_id = excluded.last_origin_node_id, "
                   "last_namespace = excluded.last_namespace, "
                   "last_cloud_seq = excluded.last_cloud_seq, "
                   "last_truck_seq = excluded.last_truck_seq");
    bind_text(stmt.get(), 1, session.local_node_id);
    bind_text(stmt.get(), 2, session.remote_node_id);
    bind_text(stmt.get(), 3, session.namespace_name);
    bind_u64(stmt.get(), 4, checkpoint.sequence_number);
    bind_blob(stmt.get(), 5, checkpoint.last_record.record_id);
    bind_text(stmt.get(), 6, checkpoint.last_record.origin_node_id);
    bind_text(stmt.get(), 7, last_namespace);
    bind_u64(stmt.get(), 8, checkpoint.last_version.cloud_seq);
    bind_u64(stmt.get(), 9, checkpoint.last_version.truck_seq);
    step_done(db, stmt.get());
}

CheckpointReadResult load_checkpoint(sqlite3* db, const SyncSessionKey& session) {
    CheckpointReadResult result;

    Statement stmt(db,
                   "SELECT sequence_number, last_record_id, last_origin_node_id, last_namespace, "
                   "last_cloud_seq, last_truck_seq FROM checkpoints "
                   "WHERE local_node_id = ? AND remote_node_id = ? AND namespace_name = ?");
    bind_text(stmt.get(), 1, session.local_node_id);
    bind_text(stmt.get(), 2, session.remote_node_id);
    bind_text(stmt.get(), 3, session.namespace_name);

    if (!step_row(db, stmt.get())) {
        return result;
    }

    result.found = true;
    result.checkpoint.sequence_number = column_u64(stmt.get(), 0);
    result.checkpoint.last_record.record_id = column_blob(stmt.get(), 1);
    result.checkpoint.last_record.origin_node_id = column_text(stmt.get(), 2);
    result.checkpoint.last_record.namespace_name = column_text(stmt.get(), 3);
    result.checkpoint.last_version.cloud_seq = column_u64(stmt.get(), 4);
    result.checkpoint.last_version.truck_seq = column_u64(stmt.get(), 5);
    return result;
}

}

SqliteCloudVehicleDbAdapter::SqliteCloudVehicleDbAdapter(DatabaseAdapterConfig config)
    : config_(std::move(config)) {
    if (config_.database_path.empty()) {
        throw std::invalid_argument("database_path must not be empty");
    }

    const std::filesystem::path db_path(config_.database_path);
    if (db_path.has_parent_path()) {
        std::filesystem::create_directories(db_path.parent_path());
    }

    if (sqlite3_open(config_.database_path.c_str(), &db_) != SQLITE_OK) {
        const std::string error = db_ == nullptr ? "sqlite3_open failed" : sqlite3_errmsg(db_);
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error(error);
    }

    initialize_schema();
}

SqliteCloudVehicleDbAdapter::~SqliteCloudVehicleDbAdapter() noexcept {
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

std::vector<CanonicalRecord> SqliteCloudVehicleDbAdapter::list_dirty_records(
    const DirtyRecordQuery& query) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string where =
        "WHERE namespace_name = ? AND ("
        "NOT EXISTS (SELECT 1 FROM remote_acks a WHERE a.local_node_id = ? AND a.remote_node_id = ? "
        "AND a.namespace_name = ? AND a.record_namespace = canonical_records.namespace_name "
        "AND a.record_origin_node_id = canonical_records.origin_node_id "
        "AND a.record_id = canonical_records.record_id) "
        "OR EXISTS (SELECT 1 FROM remote_acks a WHERE a.local_node_id = ? AND a.remote_node_id = ? "
        "AND a.namespace_name = ? AND a.record_namespace = canonical_records.namespace_name "
        "AND a.record_origin_node_id = canonical_records.origin_node_id "
        "AND a.record_id = canonical_records.record_id AND (a.ack_cloud_seq != canonical_records.cloud_seq "
        "OR a.ack_truck_seq != canonical_records.truck_seq)))";
    if (!query.include_tombstones) {
        where += " AND operation != ?";
    }

    Statement stmt(db_, records_select_sql(where, has_session_limit(query.limit)));
    bind_text(stmt.get(), 1, query.session.namespace_name);
    bind_text(stmt.get(), 2, query.session.local_node_id);
    bind_text(stmt.get(), 3, query.session.remote_node_id);
    bind_text(stmt.get(), 4, query.session.namespace_name);
    bind_text(stmt.get(), 5, query.session.local_node_id);
    bind_text(stmt.get(), 6, query.session.remote_node_id);
    bind_text(stmt.get(), 7, query.session.namespace_name);

    int next_index = 8;
    if (!query.include_tombstones) {
        bind_i32(stmt.get(), next_index++, static_cast<int>(RecordOperation::kDelete));
    }
    if (has_session_limit(query.limit)) {
        bind_u64(stmt.get(), next_index, static_cast<std::uint64_t>(query.limit));
    }

    std::vector<CanonicalRecord> records;
    while (step_row(db_, stmt.get())) {
        records.push_back(read_record_row(stmt.get()));
    }
    return records;
}

ApplyResult SqliteCloudVehicleDbAdapter::apply_record(const CanonicalRecord& record,
                                                    const std::string& idempotency_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(db_);

    const StoredLedgerEntry ledger = load_ledger(db_, idempotency_key);
    if (ledger.found) {
        ApplyResult replay;
        replay.disposition = ApplyDisposition::kDuplicate;
        replay.durable_version = ledger.durable_version;
        replay.has_persisted_conflict = ledger.has_persisted_conflict;
        if (ledger.has_persisted_conflict && ledger.conflict_id != 0) {
            const std::optional<ConflictRecord> conflict = load_conflict(db_, ledger.conflict_id);
            if (conflict.has_value()) {
                replay.persisted_conflict = *conflict;
            }
        }
        transaction.commit();
        return replay;
    }

    const std::optional<CanonicalRecord> local_record = load_record(db_, record.locator);
    const ResolveOutcome outcome = CloudVehicleSyncCore::resolve_remote_record(
        record,
        local_record.has_value() ? &*local_record : nullptr,
        owner_for(record.locator.namespace_name),
        record.locator.origin_node_id,
        conflict_detected_at_ms(record));

    ApplyResult result;
    result.disposition = outcome.disposition;
    result.durable_version = local_record.has_value() ? local_record->version_vector : VersionVector();

    if (outcome.should_apply) {
        upsert_record(db_, record, idempotency_key);
        result.durable_version = record.version_vector;
    }

    std::int64_t conflict_id = 0;
    if (outcome.should_persist_conflict) {
        conflict_id = insert_conflict(db_, outcome.conflict_record);
        result.has_persisted_conflict = true;
        result.persisted_conflict = outcome.conflict_record;
    }

    if (!outcome.should_apply && !local_record.has_value()) {
        result.durable_version = record.version_vector;
    }

    insert_ledger(db_,
                  idempotency_key,
                  record.locator,
                  outcome.disposition,
                  result.durable_version,
                  result.has_persisted_conflict,
                  conflict_id);

    transaction.commit();
    return result;
}

CheckpointReadResult SqliteCloudVehicleDbAdapter::read_checkpoint(const SyncSessionKey& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    return load_checkpoint(db_, session);
}

void SqliteCloudVehicleDbAdapter::write_checkpoint(const SyncSessionKey& session,
                                                 const CheckpointToken& checkpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(db_);

    const CheckpointReadResult current = load_checkpoint(db_, session);
    if (current.found && checkpoint.sequence_number < current.checkpoint.sequence_number) {
        transaction.commit();
        return;
    }

    store_checkpoint(db_, session, checkpoint);
    if (has_locator(checkpoint.last_record)) {
        maybe_store_remote_ack(db_, session, checkpoint.last_record, checkpoint.last_version);
    }

    transaction.commit();
}

std::uint64_t SqliteCloudVehicleDbAdapter::compute_state_checksum(const StateScope& scope) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string where = "WHERE namespace_name = ?";
    if (!scope.include_tombstones) {
        where += " AND operation != ?";
    }

    Statement stmt(db_, records_select_sql(where, false));
    bind_text(stmt.get(), 1, scope.namespace_name);
    if (!scope.include_tombstones) {
        bind_i32(stmt.get(), 2, static_cast<int>(RecordOperation::kDelete));
    }

    std::uint64_t checksum = kFnvOffsetBasis;
    while (step_row(db_, stmt.get())) {
        const CanonicalRecord record = read_record_row(stmt.get());
        mix_string(checksum, record.locator.namespace_name);
        mix_string(checksum, record.locator.origin_node_id);
        mix_bytes(checksum, record.locator.record_id);
        mix_u64(checksum, record.version_vector.cloud_seq);
        mix_u64(checksum, record.version_vector.truck_seq);
        mix_u32(checksum, static_cast<std::uint32_t>(record.operation));
        mix_bytes(checksum, record.payload);
        mix_u32(checksum, record.schema_version);
        mix_u32(checksum,
                record.operation == RecordOperation::kDelete ? static_cast<std::uint32_t>(1)
                                                             : static_cast<std::uint32_t>(0));
        mix_string(checksum, record.tombstone_reason);
    }
    return checksum;
}

std::vector<RecordLocator> SqliteCloudVehicleDbAdapter::list_record_ids(const RecordIdQuery& query) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ostringstream sql;
    sql << "SELECT namespace_name, origin_node_id, record_id FROM canonical_records WHERE namespace_name = ?";
    if (!query.include_tombstones) {
        sql << " AND operation != ?";
    }
    sql << " ORDER BY namespace_name, origin_node_id, record_id";
    if (query.limit != 0) {
        sql << " LIMIT ?";
    }

    Statement stmt(db_, sql.str());
    bind_text(stmt.get(), 1, query.namespace_name);

    int next_index = 2;
    if (!query.include_tombstones) {
        bind_i32(stmt.get(), next_index++, static_cast<int>(RecordOperation::kDelete));
    }
    if (query.limit != 0) {
        bind_u64(stmt.get(), next_index, static_cast<std::uint64_t>(query.limit));
    }

    std::vector<RecordLocator> locators;
    while (step_row(db_, stmt.get())) {
        RecordLocator locator;
        locator.namespace_name = column_text(stmt.get(), 0);
        locator.origin_node_id = column_text(stmt.get(), 1);
        locator.record_id = column_blob(stmt.get(), 2);
        locators.push_back(std::move(locator));
    }
    return locators;
}

void SqliteCloudVehicleDbAdapter::persist_conflict(const ConflictRecord& conflict) {
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction transaction(db_);
    insert_conflict(db_, conflict);
    transaction.commit();
}

std::vector<ConflictRecord> SqliteCloudVehicleDbAdapter::query_conflicts(const ConflictQuery& query) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ostringstream sql;
    sql << "SELECT id, namespace_name, origin_node_id, record_id, local_cloud_seq, local_truck_seq, "
           "remote_cloud_seq, remote_truck_seq, local_payload, remote_payload, conflict_class, "
           "detected_at_ms, correlation_id, resolved FROM conflicts "
           "WHERE namespace_name = ? AND detected_at_ms >= ?";
    if (!query.include_resolved) {
        sql << " AND resolved = 0";
    }
    sql << " ORDER BY detected_at_ms, namespace_name, origin_node_id, record_id";
    if (query.limit != 0) {
        sql << " LIMIT ?";
    }

    Statement stmt(db_, sql.str());
    bind_text(stmt.get(), 1, query.namespace_name);
    bind_u64(stmt.get(), 2, query.since_detected_at_ms);
    if (query.limit != 0) {
        bind_u64(stmt.get(), 3, static_cast<std::uint64_t>(query.limit));
    }

    std::vector<ConflictRecord> conflicts;
    while (step_row(db_, stmt.get())) {
        conflicts.push_back(read_conflict_row(stmt.get()).conflict);
    }
    return conflicts;
}

std::vector<CanonicalRecord> SqliteCloudVehicleDbAdapter::list_tombstones_for_gc(
    const TombstoneGcQuery& query) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ostringstream sql;
    sql << "SELECT namespace_name, origin_node_id, record_id, cloud_seq, truck_seq, operation, payload, "
           "schema_version, idempotency_key, correlation_id, payload_checksum, wall_clock_ms, "
           "created_at_ms, updated_at_ms, tombstone_at_ms, tombstone_reason FROM canonical_records "
           "WHERE namespace_name = ? AND operation = ? AND tombstone_at_ms > 0 AND tombstone_at_ms <= ? "
           "AND EXISTS (SELECT 1 FROM remote_acks a WHERE a.local_node_id = ? AND a.remote_node_id = ? "
           "AND a.namespace_name = ? AND a.record_namespace = canonical_records.namespace_name "
           "AND a.record_origin_node_id = canonical_records.origin_node_id "
           "AND a.record_id = canonical_records.record_id AND a.ack_cloud_seq = canonical_records.cloud_seq "
           "AND a.ack_truck_seq = canonical_records.truck_seq) "
           "ORDER BY namespace_name, origin_node_id, record_id";
    if (query.limit != 0) {
        sql << " LIMIT ?";
    }

    Statement stmt(db_, sql.str());
    bind_text(stmt.get(), 1, query.session.namespace_name);
    bind_i32(stmt.get(), 2, static_cast<int>(RecordOperation::kDelete));
    bind_u64(stmt.get(), 3, query.retention_cutoff_ms);
    bind_text(stmt.get(), 4, query.session.local_node_id);
    bind_text(stmt.get(), 5, query.session.remote_node_id);
    bind_text(stmt.get(), 6, query.session.namespace_name);
    if (query.limit != 0) {
        bind_u64(stmt.get(), 7, static_cast<std::uint64_t>(query.limit));
    }

    std::vector<CanonicalRecord> tombstones;
    while (step_row(db_, stmt.get())) {
        tombstones.push_back(read_record_row(stmt.get()));
    }
    return tombstones;
}

RecordOwner SqliteCloudVehicleDbAdapter::owner_for(const std::string& namespace_name) const {
    const auto it = config_.namespace_owners.find(namespace_name);
    if (it != config_.namespace_owners.end()) {
        return it->second;
    }
    return config_.default_owner;
}

std::uint64_t SqliteCloudVehicleDbAdapter::conflict_detected_at_ms(
    const CanonicalRecord& record) const {
    if (record.updated_at_ms != 0) {
        return record.updated_at_ms;
    }
    if (record.tombstone_at_ms != 0) {
        return record.tombstone_at_ms;
    }
    if (record.wall_clock_ms != 0) {
        return record.wall_clock_ms;
    }
    return record.created_at_ms;
}

void SqliteCloudVehicleDbAdapter::initialize_schema() {
    execute(db_, "PRAGMA journal_mode = WAL");
    execute(db_, "PRAGMA foreign_keys = ON");
    execute(db_,
            "CREATE TABLE IF NOT EXISTS canonical_records ("
            "namespace_name TEXT NOT NULL, "
            "origin_node_id TEXT NOT NULL, "
            "record_id BLOB NOT NULL, "
            "cloud_seq INTEGER NOT NULL, "
            "truck_seq INTEGER NOT NULL, "
            "operation INTEGER NOT NULL, "
            "payload BLOB NOT NULL, "
            "schema_version INTEGER NOT NULL, "
            "idempotency_key TEXT NOT NULL, "
            "correlation_id TEXT NOT NULL, "
            "payload_checksum INTEGER NOT NULL, "
            "wall_clock_ms INTEGER NOT NULL, "
            "created_at_ms INTEGER NOT NULL, "
            "updated_at_ms INTEGER NOT NULL, "
            "tombstone_at_ms INTEGER NOT NULL, "
            "tombstone_reason TEXT NOT NULL, "
            "PRIMARY KEY(namespace_name, origin_node_id, record_id))");
    execute(db_,
            "CREATE TABLE IF NOT EXISTS checkpoints ("
            "local_node_id TEXT NOT NULL, "
            "remote_node_id TEXT NOT NULL, "
            "namespace_name TEXT NOT NULL, "
            "sequence_number INTEGER NOT NULL, "
            "last_record_id BLOB NOT NULL, "
            "last_origin_node_id TEXT NOT NULL, "
            "last_namespace TEXT NOT NULL, "
            "last_cloud_seq INTEGER NOT NULL, "
            "last_truck_seq INTEGER NOT NULL, "
            "PRIMARY KEY(local_node_id, remote_node_id, namespace_name))");
    execute(db_,
            "CREATE TABLE IF NOT EXISTS remote_acks ("
            "local_node_id TEXT NOT NULL, "
            "remote_node_id TEXT NOT NULL, "
            "namespace_name TEXT NOT NULL, "
            "record_namespace TEXT NOT NULL, "
            "record_origin_node_id TEXT NOT NULL, "
            "record_id BLOB NOT NULL, "
            "ack_cloud_seq INTEGER NOT NULL, "
            "ack_truck_seq INTEGER NOT NULL, "
            "PRIMARY KEY(local_node_id, remote_node_id, namespace_name, record_namespace, "
            "record_origin_node_id, record_id))");
    execute(db_,
            "CREATE TABLE IF NOT EXISTS conflicts ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "namespace_name TEXT NOT NULL, "
            "origin_node_id TEXT NOT NULL, "
            "record_id BLOB NOT NULL, "
            "local_cloud_seq INTEGER NOT NULL, "
            "local_truck_seq INTEGER NOT NULL, "
            "remote_cloud_seq INTEGER NOT NULL, "
            "remote_truck_seq INTEGER NOT NULL, "
            "local_payload BLOB NOT NULL, "
            "remote_payload BLOB NOT NULL, "
            "conflict_class INTEGER NOT NULL, "
            "detected_at_ms INTEGER NOT NULL, "
            "correlation_id TEXT NOT NULL, "
            "resolved INTEGER NOT NULL DEFAULT 0)");
    execute(db_,
            "CREATE TABLE IF NOT EXISTS idempotency_ledger ("
            "idempotency_key TEXT PRIMARY KEY, "
            "namespace_name TEXT NOT NULL, "
            "origin_node_id TEXT NOT NULL, "
            "record_id BLOB NOT NULL, "
            "durable_cloud_seq INTEGER NOT NULL, "
            "durable_truck_seq INTEGER NOT NULL, "
            "apply_outcome INTEGER NOT NULL, "
            "has_persisted_conflict INTEGER NOT NULL, "
            "conflict_id INTEGER)");
}

}
}
