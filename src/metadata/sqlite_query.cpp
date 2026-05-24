// SPDX-License-Identifier: GPL-3.0-or-later
#include "sqlite_query.h"

#include <sqlite3.h>

#include <string>
#include <utility>

namespace chd::metadata {

SqliteQuery::SqliteQuery() = default;

SqliteQuery::SqliteQuery(sqlite3 *db) : db_(db) {}

SqliteQuery::SqliteQuery(SqliteQuery &&other) noexcept
    : db_(other.db_),
      stmt_(other.stmt_),
      nextBindIndex_(other.nextBindIndex_),
      lastError_(std::move(other.lastError_)) {
    other.db_ = nullptr;
    other.stmt_ = nullptr;
    other.nextBindIndex_ = 1;
}

SqliteQuery &SqliteQuery::operator=(SqliteQuery &&other) noexcept {
    if (this != &other) {
        finish();
        db_ = other.db_;
        stmt_ = other.stmt_;
        nextBindIndex_ = other.nextBindIndex_;
        lastError_ = std::move(other.lastError_);
        other.db_ = nullptr;
        other.stmt_ = nullptr;
        other.nextBindIndex_ = 1;
    }
    return *this;
}

SqliteQuery::~SqliteQuery() { finish(); }

bool SqliteQuery::prepareInternal(const std::string &sql) {
    if (db_ == nullptr) {
        lastError_ = Error("SqliteQuery has no database connection");
        return false;
    }
    if (stmt_) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
    nextBindIndex_ = 1;
    const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        lastError_ = Error(sqlite3_errmsg(db_));
        stmt_ = nullptr;
        return false;
    }
    lastError_ = Error();
    return true;
}

bool SqliteQuery::prepare(const std::string &sql) {
    return prepareInternal(sql);
}

void SqliteQuery::addBindValue(int value) {
    sqlite3_bind_int(stmt_, nextBindIndex_++, value);
}

void SqliteQuery::addBindValue(int64_t value) {
    sqlite3_bind_int64(stmt_, nextBindIndex_++, value);
}

void SqliteQuery::addBindValue(double value) {
    sqlite3_bind_double(stmt_, nextBindIndex_++, value);
}

void SqliteQuery::addBindValue(const std::string &value) {
    sqlite3_bind_text(stmt_, nextBindIndex_++, value.c_str(), -1, SQLITE_TRANSIENT);
}

void SqliteQuery::addBindValue(const char *value) {
    sqlite3_bind_text(stmt_, nextBindIndex_++, value, -1, SQLITE_TRANSIENT);
}

void SqliteQuery::addBindNull() {
    sqlite3_bind_null(stmt_, nextBindIndex_++);
}

void SqliteQuery::addBindValueOrNull(const std::string &value) {
    if (value.empty()) addBindNull();
    else addBindValue(value);
}

bool SqliteQuery::exec() {
    if (stmt_ == nullptr) {
        lastError_ = Error("exec() called on null statement");
        return false;
    }
    // For row-returning statements (column_count > 0), defer stepping to
    // next(); the caller iterates rows via while (q.next()). For non-row
    // statements (INSERT/UPDATE/DELETE/CREATE), step once to completion.
    if (sqlite3_column_count(stmt_) > 0) {
        return true;
    }
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_DONE) return true;
    lastError_ = Error(sqlite3_errmsg(db_));
    return false;
}

bool SqliteQuery::exec(const std::string &sql) {
    if (!prepareInternal(sql)) return false;
    return exec();
}

bool SqliteQuery::next() {
    if (stmt_ == nullptr) return false;
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    lastError_ = Error(sqlite3_errmsg(db_));
    return false;
}

void SqliteQuery::reset() {
    if (stmt_) {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
    nextBindIndex_ = 1;
}

void SqliteQuery::finish() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
    db_ = nullptr;
    nextBindIndex_ = 1;
}

int SqliteQuery::columnCount() const {
    return stmt_ ? sqlite3_column_count(stmt_) : 0;
}

int SqliteQuery::columnIndexByName(const char *name) const {
    if (stmt_ == nullptr) return -1;
    const int n = sqlite3_column_count(stmt_);
    for (int i = 0; i < n; i++) {
        const char *colName = sqlite3_column_name(stmt_, i);
        if (colName && std::string(colName) == name) return i;
    }
    return -1;
}

SqliteQuery::Value SqliteQuery::value(const char *columnName) const {
    return Value(stmt_, columnIndexByName(columnName));
}

SqliteQuery::Value SqliteQuery::value(int columnIndex) const {
    return Value(stmt_, columnIndex);
}

SqliteQuery::LastInsertId SqliteQuery::lastInsertId() const {
    return LastInsertId(db_ ? sqlite3_last_insert_rowid(db_) : 0);
}

bool SqliteQuery::Value::isNull() const {
    if (column_ < 0 || stmt_ == nullptr) return true;
    return sqlite3_column_type(stmt_, column_) == SQLITE_NULL;
}

int SqliteQuery::Value::toInt() const {
    if (column_ < 0 || stmt_ == nullptr) return 0;
    return sqlite3_column_int(stmt_, column_);
}

int64_t SqliteQuery::Value::toLongLong() const {
    if (column_ < 0 || stmt_ == nullptr) return 0;
    return sqlite3_column_int64(stmt_, column_);
}

double SqliteQuery::Value::toDouble() const {
    if (column_ < 0 || stmt_ == nullptr) return 0.0;
    return sqlite3_column_double(stmt_, column_);
}

std::string SqliteQuery::Value::toString() const {
    if (column_ < 0 || stmt_ == nullptr) return "";
    const unsigned char *text = sqlite3_column_text(stmt_, column_);
    if (text == nullptr) return "";
    return reinterpret_cast<const char *>(text);
}

bool SqliteQuery::Value::toBool() const {
    return toInt() == 1;
}

namespace SqliteValue {

int toIntOrDefault(const SqliteQuery &query, const char *column, int defaultValue) {
    const auto value = query.value(column);
    return value.isNull() ? defaultValue : value.toInt();
}

int64_t toLongLongOrDefault(const SqliteQuery &query, const char *column, int64_t defaultValue) {
    const auto value = query.value(column);
    return value.isNull() ? defaultValue : value.toLongLong();
}

double toDoubleOrDefault(const SqliteQuery &query, const char *column, double defaultValue) {
    const auto value = query.value(column);
    return value.isNull() ? defaultValue : value.toDouble();
}

bool toBoolOrDefault(const SqliteQuery &query, const char *column, bool defaultValue) {
    const auto value = query.value(column);
    return value.isNull() ? defaultValue : (value.toInt() == 1);
}

}  // namespace SqliteValue

}  // namespace chd::metadata
