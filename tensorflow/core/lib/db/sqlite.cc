/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/lib/db/sqlite.h"

#include "tensorflow/core/lib/io/record_reader.h"
#include "tensorflow/core/platform/logging.h"

namespace tensorflow {

/* static */
xla::StatusOr<std::shared_ptr<Sqlite>> Sqlite::Open(const string& uri) {
  sqlite3* sqlite = nullptr;
  Status s = MakeStatus(sqlite3_open(uri.c_str(), &sqlite));
  if (s.ok()) {
    return std::shared_ptr<Sqlite>(new Sqlite(sqlite));
  }
  return s;
}

/* static */ Status Sqlite::MakeStatus(int resultCode) {
  // See: https://sqlite.org/rescode.html
  switch (resultCode & 0xff) {
    case SQLITE_OK:
    case SQLITE_ROW:   // sqlite3_step() has another row ready
    case SQLITE_DONE:  // sqlite3_step() has finished executing
      return Status::OK();
    case SQLITE_ABORT:  // Callback routine requested an abort
      return errors::Aborted(sqlite3_errstr(resultCode));
    case SQLITE_READONLY:  // Attempt to write a readonly database
    case SQLITE_MISMATCH:  // Data type mismatch
      return errors::FailedPrecondition(sqlite3_errstr(resultCode));
    case SQLITE_MISUSE:    // Library used incorrectly
    case SQLITE_INTERNAL:  // Internal logic error in SQLite
      return errors::Internal(sqlite3_errstr(resultCode));
    case SQLITE_RANGE:  // 2nd parameter to sqlite3_bind out of range
      return errors::OutOfRange(sqlite3_errstr(resultCode));
    case SQLITE_CANTOPEN:    // Unable to open the database file
    case SQLITE_CONSTRAINT:  // Abort due to constraint violation
    case SQLITE_NOTFOUND:    // Unknown opcode or statement parameter name
    case SQLITE_NOTADB:      // File opened that is not a database file
      return errors::InvalidArgument(sqlite3_errstr(resultCode));
    case SQLITE_CORRUPT:  // The database disk image is malformed
      return errors::DataLoss(sqlite3_errstr(resultCode));
    case SQLITE_AUTH:  // Authorization denied
    case SQLITE_PERM:  // Access permission denied
      return errors::PermissionDenied(sqlite3_errstr(resultCode));
    case SQLITE_FULL:    // Insertion failed because database is full
    case SQLITE_TOOBIG:  // String or BLOB exceeds size limit
    case SQLITE_NOLFS:   // Uses OS features not supported on host
      return errors::ResourceExhausted(sqlite3_errstr(resultCode));
    case SQLITE_BUSY:      // The database file is locked
    case SQLITE_LOCKED:    // A table in the database is locked
    case SQLITE_PROTOCOL:  // Database lock protocol error
    case SQLITE_NOMEM:     // A malloc() failed
      return errors::Unavailable(sqlite3_errstr(resultCode));
    case SQLITE_INTERRUPT:  // Operation terminated by sqlite3_interrupt
      return errors::Cancelled(sqlite3_errstr(resultCode));
    case SQLITE_ERROR:   // SQL error or missing database
    case SQLITE_IOERR:   // Some kind of disk I/O error occurred
    case SQLITE_SCHEMA:  // The database schema changed
    default:
      return errors::Unknown(sqlite3_errstr(resultCode));
  }
}

Sqlite::Sqlite(sqlite3* db) : db_(db) {}

Sqlite::~Sqlite() {
  // close_v2 doesn't care if a stmt hasn't been GC'd yet
  int rc = sqlite3_close_v2(db_);
  if (rc != SQLITE_OK) {
    LOG(ERROR) << "destruct sqlite3: " << MakeStatus(rc);
  }
}

Status Sqlite::Close() {
  if (db_ == nullptr) {
    return Status::OK();
  }
  // If Close is explicitly called, ordering must be correct.
  Status s = MakeStatus(sqlite3_close(db_));
  if (s.ok()) {
    db_ = nullptr;
  }
  return s;
}

SqliteStatement Sqlite::Prepare(const string& sql) {
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), sql.size() + 1, &stmt, nullptr);
  if (rc == SQLITE_OK) {
    return {stmt, SQLITE_OK, std::unique_ptr<string>(nullptr)};
  } else {
    return {nullptr, rc, std::unique_ptr<string>(new string(sql))};
  }
}

Status SqliteStatement::status() const {
  Status s = Sqlite::MakeStatus(error_);
  if (!s.ok()) {
    if (stmt_ != nullptr) {
      errors::AppendToMessage(&s, sqlite3_sql(stmt_));
    } else {
      errors::AppendToMessage(&s, *prepare_error_sql_);
    }
  }
  return s;
}

void SqliteStatement::CloseOrLog() {
  if (stmt_ != nullptr) {
    int rc = sqlite3_finalize(stmt_);
    if (rc != SQLITE_OK) {
      LOG(ERROR) << "destruct sqlite3_stmt: " << Sqlite::MakeStatus(rc);
    }
    stmt_ = nullptr;
  }
}

Status SqliteStatement::Close() {
  if (stmt_ == nullptr) {
    return Status::OK();
  }
  int rc = sqlite3_finalize(stmt_);
  if (rc == SQLITE_OK) {
    stmt_ = nullptr;
  }
  Update(rc);
  return status();
}

void SqliteStatement::Reset() {
  if (TF_PREDICT_TRUE(stmt_ != nullptr)) {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);  // not nullptr friendly
  }
  error_ = SQLITE_OK;
}

Status SqliteStatement::Step(bool* isDone) {
  if (TF_PREDICT_FALSE(error_ != SQLITE_OK)) {
    *isDone = true;
    return status();
  }
  int rc = sqlite3_step(stmt_);
  switch (rc) {
    case SQLITE_ROW:
      *isDone = false;
      return Status::OK();
    case SQLITE_DONE:
      *isDone = true;
      return Status::OK();
    default:
      *isDone = true;
      error_ = rc;
      return status();
  }
}

Status SqliteStatement::StepAndReset() {
  if (TF_PREDICT_FALSE(error_ != SQLITE_OK)) {
    return status();
  }
  Status s;
  int rc = sqlite3_step(stmt_);
  if (rc != SQLITE_DONE) {
    if (rc == SQLITE_ROW) {
      s.Update(errors::Internal("unexpected sqlite row"));
    } else {
      s.Update(Sqlite::MakeStatus(rc));
    }
  }
  Reset();
  return s;
}

}  // namespace tensorflow
