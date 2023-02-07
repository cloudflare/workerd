// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/filesystem.h>
#include <kj/async.h>
#include <kj/one-of.h>

struct sqlite3;
struct sqlite3_vfs;
struct sqlite3_stmt;

KJ_DECLARE_NON_POLYMORPHIC(sqlite3_stmt);

namespace workerd {

using kj::byte;
using kj::uint;

class Sqlite {
  // C++/KJ API for SQLite.
  //
  // In addition to providing a more modern C++ interface vs. the classic C API, this API layers
  // SQLite on top of KJ's filesystem API. This means that you can use KJ's in-memory filesystem
  // implementation in unit tests. Meanwhile, though, if you do actually give it a `kj::Directory`
  // representing a true disk directory, the real SQLite disk implementation will be used with
  // all of its features.

public:
  class Vfs;
  class Query;
  class Statement;

  Sqlite(Vfs& vfs, kj::PathPtr path);
  Sqlite(Vfs& vfs, kj::PathPtr path, kj::WriteMode mode);
  ~Sqlite() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Sqlite);

  operator sqlite3*() { return db; }
  // Allows a Sqlite object to be passed directly into SQLite API functions where `sqlite*` is
  // expected.

  Statement prepare(kj::StringPtr sqlCode);
  // Prepares the given SQL code as a persistent statement that can be used across several queries.
  // Don't use this for one-off queries; pass the code to the Query constructor.

  template <typename... Params>
  Query run(kj::StringPtr sqlCode, Params&&... bindings);
  // Convenience method to start a query. This is equivalent to `prepare(sqlCode).run(bindings...)`
  // but may be more efficient for the one-off use caes.

private:
  sqlite3* db;

  void close();
};

class Sqlite::Statement {
  // Represents a prepared SQL statement, which can be executed many times.

public:
  template <typename... Params>
  Query run(Params&&... bindings);
  // Convenience method to start a query. This is equivalent to:
  //
  //     Sqlite::Query(db, statement, bindings...);

  operator sqlite3_stmt*() { return stmt; }

private:
  Sqlite& db;
  kj::Own<sqlite3_stmt> stmt;

  Statement(Sqlite& db, kj::Own<sqlite3_stmt> stmt): db(db), stmt(kj::mv(stmt)) {}

  friend class Sqlite;
};

class Sqlite::Query {
  // Represents one SQLite query.
  //
  // Only one Query can exist at a time, for a given database. It should probably be allocated on
  // the stack.

public:
  using ValuePtr = kj::OneOf<kj::ArrayPtr<const byte>, kj::StringPtr, int64_t, double,
                             decltype(nullptr)>;

  Query(Sqlite& db, Statement& statement, kj::ArrayPtr<const ValuePtr> bindings);
  // Begin a query executing a prepared statement.
  //
  // `bindings` are the value to fill into `?`s in the statement. The `bindings` array itself
  // need only live until the constructor returns, but any strings or blobs it points to must
  // remain valid until the Query is destroyed.

  Query(Sqlite& db, kj::StringPtr sqlCode, kj::ArrayPtr<const ValuePtr> bindings);
  // Begin a one-off query executing some code directly.

  template <typename... Params>
  Query(Sqlite& db, Statement& statement, Params&&... bindings)
      : Query(db, statement, {ValuePtr(bindings)...}) {}
  template <typename... Params>
  Query(Sqlite& db, kj::StringPtr sqlCode, Params&&... bindings)
      : Query(db, sqlCode, {ValuePtr(bindings)...}) {}
  // These versions of the constructor accept the binding values as positional parameters. This
  // may be convenient when the number of bindings is statically known.

  ~Query() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Query);

  bool isDone() { return done; }
  // If true, there are no more rows. (When true, the methods below must not be called.)

  uint changeCount();
  // For INSERT, UPDATE, or DELETE queries, returns the number of rows changed. For other query
  // types the result is undefined.

  void nextRow();
  // Advance to the next row.

  uint columnCount();
  // How many columns does each row of the result have?

  ValuePtr getValue(uint column);
  // Get the value at the given column, as whatever type was actually returned.
  //
  // Returned pointers (strings and blobs) remain valid only until either (a) nextRow() is called,
  // or (b) a different get method is called on the same column but with a different type.

  kj::ArrayPtr<const byte> getBlob(uint column);
  kj::StringPtr getText(uint column);
  int getInt(uint column);
  int64_t getInt64(uint column);
  double getDouble(uint column);
  bool isNull(uint column);
  // Get the value at the given column, coercing it to the desired type according to SQLite rules.

  kj::Maybe<kj::ArrayPtr<const byte>> getMaybeBlob(uint column) {
    if (isNull(column)) { return nullptr; } else { return getBlob(column); }
  }
  kj::Maybe<kj::StringPtr> getMaybeText(uint column) {
    if (isNull(column)) { return nullptr; } else { return getText(column); }
  }
  kj::Maybe<int> getMaybeInt(uint column) {
    if (isNull(column)) { return nullptr; } else { return getInt(column); }
  }
  kj::Maybe<int64_t> getMaybeInt64(uint column) {
    if (isNull(column)) { return nullptr; } else { return getInt64(column); }
  }
  kj::Maybe<double> getMaybeDouble(uint column) {
    if (isNull(column)) { return nullptr; } else { return getDouble(column); }
  }

private:
  sqlite3* db;
  kj::Own<sqlite3_stmt> ownStatement;   // for one-off queries
  sqlite3_stmt* statement;
  bool done = false;

  void init(kj::ArrayPtr<const ValuePtr> bindings);
};

class Sqlite::Vfs {
  // Implements a SQLite VFS based on a KJ directory.
  //
  // If the directory is detected to be a disk directory (i.e. getFd() or getWin32Handle() returns
  // non-null), this VFS implementation will actually delegate to the built-in one. This ensures
  // feature-parity for production use.
  //
  // If the directory is not a disk directory, then the VFS will actually use the KJ APIs, but
  // some features will be missing. Most importantly, as of this writing, KJ filesystem APIs do
  // not support locks, so all locking will be ignored.

public:
  explicit Vfs(const kj::Directory& directory);
  ~Vfs() noexcept(false);

  kj::StringPtr getName() { return name; }
  // Unfortunately, all SQLite VFSes must be registered in a global list with unique names, and
  // then the _name_ must be passed to sqlite3_open_v2() to use it when opening a database. This is
  // dumb, you should instead be able to simply pass the sqlite3_vfs* when opening the database,
  // but this is the way it is. To work around this, each VFS is assigned an auto-generated unique
  // name.
  //
  // TODO(cleanup): Patch SQLite to allow passing the pointer in?

  KJ_DISALLOW_COPY_AND_MOVE(Vfs);

private:
  const kj::Directory& directory;
  kj::String name;

  sqlite3_vfs& native;  // the system's default VFS implementation
  kj::Own<sqlite3_vfs> vfs;  // our VFS

  int rootFd = -1;
  // Result of `directory.getFd()`, if it returns non-null. Cached here for convenience.

  template <typename T, T slot>
  struct MethodWrapperHack;

  struct WrappedNativeFileImpl;
  sqlite3_vfs makeWrappedNativeVfs();
  // Create a VFS definition that wraps the native VFS implementation except that it treats our
  // `directory` as the root. Requires that the directory is a real disk directory (and `rootFd`
  // is filled in).

  struct FileImpl;
  sqlite3_vfs makeKjVfs();
  // Create a VFS definition that actually delegates to the KJ filesystem.

  friend class Sqlite;
};

template <typename... Params>
Sqlite::Query Sqlite::run(kj::StringPtr sqlCode, Params&&... params) {
  return Query(*this, sqlCode, kj::fwd<Params>(params)...);
}

template <typename... Params>
Sqlite::Query Sqlite::Statement::run(Params&&... params) {
  return Query(db, *this, kj::fwd<Params>(params)...);
}

}  // namespace workerd
