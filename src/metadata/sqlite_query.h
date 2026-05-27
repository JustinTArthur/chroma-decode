// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace chd::metadata {

// RAII owner for a sqlite3 database handle. Destructor uses
// sqlite3_close_v2 (deferred close) so any prepared statements that
// outlive an explicit close — e.g. stack-local SqliteQuery objects
// destroyed during return-path unwinding — finalize cleanly rather
// than leaking SQLite's internal pages. handle() is the only accessor;
// no implicit conversion to sqlite3*, to keep `sqlite3_close(db)` from
// compiling at call sites and re-introducing the same bug class.
class SqliteDb {
public:
    SqliteDb() noexcept = default;
    explicit SqliteDb(sqlite3 *raw) noexcept : db_(raw) {}
    ~SqliteDb() noexcept { close(); }

    SqliteDb(const SqliteDb &) = delete;
    SqliteDb &operator=(const SqliteDb &) = delete;
    SqliteDb(SqliteDb &&other) noexcept : db_(other.db_) { other.db_ = nullptr; }
    SqliteDb &operator=(SqliteDb &&other) noexcept {
        if (this != &other) {
            close();
            db_ = other.db_;
            other.db_ = nullptr;
        }
        return *this;
    }

    // sqlite3_open_v2(path, &out, flags, nullptr). Returns the SQLite
    // error code (SQLITE_OK on success). On error, if SQLite allocated a
    // handle so that errmsg() is meaningful, it is still owned and will
    // be closed by the destructor.
    int open(const std::string &path, int flags);

    // sqlite3_close_v2. Idempotent.
    void close() noexcept;

    bool isOpen() const noexcept { return db_ != nullptr; }
    sqlite3 *handle() const noexcept { return db_; }
    std::string errmsg() const;

private:
    sqlite3 *db_ = nullptr;
};

// QSqlQuery-shaped adapter around a sqlite3 prepared statement.
// Designed so call sites ported from QSqlQuery keep the same shape:
//
//     SqliteQuery query(db);
//     query.prepare("INSERT ... VALUES (?, ?)");
//     query.addBindValue(captureId);
//     query.addBindValue(fieldId);
//     if (!query.exec()) { /* handle query.lastError().text() */ }
//
class SqliteQuery {
public:
    // Mirrors QSqlError's interface so call sites' .lastError().text() works.
    class Error {
    public:
        explicit Error(std::string text = {}) : text_(std::move(text)) {}
        const std::string &text() const { return text_; }
    private:
        std::string text_;
    };

    SqliteQuery();
    explicit SqliteQuery(sqlite3 *db);
    explicit SqliteQuery(const SqliteDb &db);
    SqliteQuery(const SqliteQuery &) = delete;
    SqliteQuery &operator=(const SqliteQuery &) = delete;
    SqliteQuery(SqliteQuery &&other) noexcept;
    SqliteQuery &operator=(SqliteQuery &&other) noexcept;
    ~SqliteQuery();

    // Prepare a statement on the stored database; throws std::runtime_error
    // if no db is associated, returns false if compilation fails.
    bool prepare(const std::string &sql);

    // Positional bind helpers (next index auto-advances starting at 1, like
    // QSqlQuery::addBindValue).
    void addBindValue(int value);
    void addBindValue(int64_t value);
    void addBindValue(double value);
    void addBindValue(const std::string &value);
    void addBindValue(const char *value);
    void addBindNull();

    // Bind text (or NULL if empty). Matches the Qt idiom
    // `value.isEmpty() ? QVariant() : value`.
    void addBindValueOrNull(const std::string &value);

    // Execute a non-row-returning statement (INSERT/UPDATE/DELETE) or a
    // SELECT (call next() to iterate rows after a successful exec).
    bool exec();

    // Execute an arbitrary SQL string (resets+prepares as needed).
    bool exec(const std::string &sql);

    // Step to the next row. Returns true if a row is available.
    bool next();

    // Reset the statement so it can be re-executed (clears binds).
    void reset();

    // Release the underlying statement.
    void finish();

    // Number of columns in the current result set.
    int columnCount() const;

    class Value {
    public:
        Value(sqlite3_stmt *stmt, int column) : stmt_(stmt), column_(column) {}

        bool isNull() const;
        int toInt() const;
        int64_t toLongLong() const;
        double toDouble() const;
        std::string toString() const;
        bool toBool() const;  // 1 -> true, 0/null -> false
    private:
        sqlite3_stmt *stmt_;
        int column_;
    };

    Value value(const char *columnName) const;
    Value value(int columnIndex) const;

    // Wraps sqlite3_last_insert_rowid().
    class LastInsertId {
    public:
        explicit LastInsertId(int64_t id) : id_(id) {}
        int toInt() const { return static_cast<int>(id_); }
        int64_t toLongLong() const { return id_; }
    private:
        int64_t id_;
    };
    LastInsertId lastInsertId() const;

    const Error &lastError() const { return lastError_; }

    sqlite3 *db() const { return db_; }
    sqlite3_stmt *raw() const { return stmt_; }
    bool isValid() const { return stmt_ != nullptr; }

private:
    sqlite3 *db_ = nullptr;
    sqlite3_stmt *stmt_ = nullptr;
    int nextBindIndex_ = 1;
    Error lastError_;

    int columnIndexByName(const char *name) const;
    bool prepareInternal(const std::string &sql);
};

// Legacy SqliteValue free-function helpers; signatures preserved from
// ld-decode-tools sqliteio.cpp so call sites in core.cpp remain unchanged.
namespace SqliteValue {
    int toIntOrDefault(const SqliteQuery &query, const char *column, int defaultValue = -1);
    int64_t toLongLongOrDefault(const SqliteQuery &query, const char *column, int64_t defaultValue = -1);
    double toDoubleOrDefault(const SqliteQuery &query, const char *column, double defaultValue = -1.0);
    bool toBoolOrDefault(const SqliteQuery &query, const char *column, bool defaultValue = false);
}

}  // namespace chd::metadata
