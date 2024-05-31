// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/filesystem.h>
#include <kj/one-of.h>
#include <utility>

struct sqlite3;
struct sqlite3_vfs;
struct sqlite3_stmt;

KJ_DECLARE_NON_POLYMORPHIC(sqlite3_stmt);

namespace workerd {

using kj::byte;
using kj::uint;

// C++/KJ API for SQLite.
//
// In addition to providing a more modern C++ interface vs. the classic C API, this API layers
// SQLite on top of KJ's filesystem API. This means that you can use KJ's in-memory filesystem
// implementation in unit tests. Meanwhile, though, if you do actually give it a `kj::Directory`
// representing a true disk directory, the real SQLite disk implementation will be used with
// all of its features.
class SqliteDatabase {
public:
  class Vfs;
  class Query;
  class Statement;
  class Lock;
  class LockManager;
  class Regulator;
  struct VfsOptions;

  struct IngestResult {
    kj::StringPtr remainder;
    uint64_t rowsRead;
    uint64_t rowsWritten;
    uint64_t statementCount;
  };

  SqliteDatabase(const Vfs& vfs, kj::PathPtr path, kj::Maybe<kj::WriteMode> maybeMode = kj::none);
  ~SqliteDatabase() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(SqliteDatabase);

  // Allows a SqliteDatabase to be passed directly into SQLite API functions where `sqlite*` is
  // expected.
  operator sqlite3*() { return db; }

  // Use as the `Regulator&` for queries that are fully trusted. As a general rule, this should
  // be used if and only if the SQL query is a string literal.
  static Regulator TRUSTED;

  // Prepares the given SQL code as a persistent statement that can be used across several queries.
  // Don't use this for one-off queries; pass the code to the Query constructor.
  Statement prepare(Regulator& regulator, kj::StringPtr sqlCode);

  // Convenience method to start a query. This is equivalent to `prepare(sqlCode).run(bindings...)`
  // except:
  // - It may be more efficient for one-off use caes.
  // - The code can include multiple statements, separated by semicolons. The bindings and returned
  //   `Query` object are both associated with the last statement. This is particularly convenient
  //   for doing database initialization such as creating several tables at once.
  template <typename... Params>
  Query run(Regulator& regulator, kj::StringPtr sqlCode, Params&&... bindings);

  template <size_t size>
  Statement prepare(const char (&sqlCode)[size]);

  // When the input is a string literal, we automatically use the TRUSTED regulator.
  template <size_t size, typename... Params>
  Query run(const char (&sqlCode)[size], Params&&... bindings);

  // Invokes the given callback whenever a query begins which may write to the database. The
  // callback is called just before executing the query.
  //
  // Durable Objects uses this to automatically begin a transaction and close the output gate.
  void onWrite(kj::Function<void()> callback) { onWriteCallback = kj::mv(callback); }

  // Invoke the onWrite() callback.
  //
  // This is useful when the caller is about to execute a statement which SQLite considers
  // read-only, but needs to be considered a write for our purposes. In particular, we use the
  // onWrite callback to start automatic transactions, and we use the SAVEPOINT statement to
  // implement explicit transactions. For synchronous transactions, the explicit transaction needs
  // to be nested inside the automatic transaction, so we need to force an auto-transaction to
  // start before the SAVEPOINT.
  void notifyWrite();

  // Get the currently-executing SQL query for debug purposes. The query is normalized to hide
  // any literal values that might contain sensitive information. This is intended to be safe for
  // debug logs.
  kj::StringPtr getCurrentQueryForDebug();

  // Helper to execute a chunk of SQL that may not be complete.
  // Executes every valid statement provided, and returns the remaining portion of the input
  // that was not processed. This is used for streaming SQL ingestion.
  IngestResult ingestSql(Regulator& regulator, kj::StringPtr sqlCode);

  // Execute a function with the given regulator.
  void executeWithRegulator(Regulator& regulator, kj::FunctionParam<void()> func);

private:
  sqlite3* db;

  // Set while a query is compiling.
  kj::Maybe<Regulator&> currentRegulator;

  // Set while a statement is executing.
  kj::Maybe<sqlite3_stmt&> currentStatement;

  kj::Maybe<kj::Function<void()>> onWriteCallback;

  void close();

  enum Multi { SINGLE, MULTI };

  // Helper to call sqlite3_prepare_v3().
  //
  // In SINGLE mode, an exception is thrown if `sqlCode` contains multiple statements.
  //
  // In MULTI mode, if `sqlCode` contains multiple statements, each statement before the last one
  // is executed immediately. The returned object represents the last statement.
  kj::Own<sqlite3_stmt> prepareSql(
      Regulator& regulator, kj::StringPtr sqlCode, uint prepFlags, Multi multi);

  // Implements SQLite authorizer callback, see sqlite3_set_authorizer().
  bool isAuthorized(int actionCode,
      kj::Maybe<kj::StringPtr> param1, kj::Maybe<kj::StringPtr> param2,
      kj::Maybe<kj::StringPtr> dbName, kj::Maybe<kj::StringPtr> triggerName);

  // Implements SQLite authorizer for 'temp' DB
  bool isAuthorizedTemp(int actionCode,
      const kj::Maybe <kj::StringPtr> &param1, const kj::Maybe <kj::StringPtr> &param2,
      Regulator &regulator);

  void setupSecurity();
};

// Class which regulates a SQL query, especially to control how queries created in JavaScript
// application code are handled.
class SqliteDatabase::Regulator {

public:
  // Returns whether the given name (which may be a table, index, view, etc.) is allowed to be
  // accessed. Typically, this is used to deny access to names containing special prefixes
  // indicating that they are privileged, like `_cf_`.
  //
  // This only applies to global names. Scoped names, such as column names, are not subject to
  // authorization.
  virtual bool isAllowedName(kj::StringPtr name) { return true; }

  // Returns whether a given trigger or view name should be permitted to run as a side effect of a
  // query running under this Regulator. This is a precaution to prevent application-defined
  // triggers from executing under a privileged regulator.
  //
  // TODO(someday): In theory a trigger should run with the authority level under which it was
  //   created, but how do we track that? In practice we probably never expect triggers to run on
  //   trusted queries.
  virtual bool isAllowedTrigger(kj::StringPtr name) { return false; }

  // Report that an error occurred. `message` is the detail message constructed by SQLite. This
  // function should typically throw an exception. If no exception is thrown, a simple KJ exception
  // will be thrown after `onError()` returns.
  //
  // The purpose of this callback is to allow the JavaScript API bindings to throw a JSG exception.
  //
  // Note that SQLITE_MISUSE errors are NOT reported using `onError()` -- they will throw regular
  // KJ exceptions in all cases. This is because SQLITE_MISUSE indicates a bug that could lead to
  // undefined behavior. Such bugs are always in C++ code; JavaScript application code must be
  // prohibited from causing such errors in the first place.
  virtual void onError(kj::StringPtr message) {}

  // Are BEGIN TRANSACTION and SAVEPOINT statements allowed? Note that if allowed, SAVEPOINT will
  // also be subject to `isAllowedName()` for the savepoint name. If denied, the application will
  // not be able to create any sort of transaction.
  //
  // In Durable Objects, we disallow these statements because the platform provides an explicit
  // API for transactions that is safer (e.g. it automatically rolls back on throw). Also, the
  // platform automatically wraps every entry into the isolate lock in a transaction.
  virtual bool allowTransactions() { return true; }
};

// Represents a prepared SQL statement, which can be executed many times.
class SqliteDatabase::Statement {
public:
  // Convenience method to start a query. This is equivalent to:
  //
  //     SqliteDatabase::Query(db, statement, bindings...);
  //
  // `bindings` are the values to fill into `?`s in the statement. Each value in `bindings` must
  // be one of the types of Query::ValuePtr. Alternatively, `bindings` can be a single parameter
  // of type `ArrayPtr<const Query::ValuePtr>` to initialize bindings from an array.
  //
  // Any strings or byte blobs in the bindings must remain valid until the `Query` is destroyed.
  // However, when passing `bindings` as an array, the outer array need only remain valid until
  // this method returns.
  template <typename... Params>
  Query run(Params&&... bindings);

  operator sqlite3_stmt*() { return stmt; }

private:
  SqliteDatabase& db;
  Regulator& regulator;
  kj::Own<sqlite3_stmt> stmt;

  Statement(SqliteDatabase& db, Regulator& regulator, kj::Own<sqlite3_stmt> stmt)
      : db(db), regulator(regulator), stmt(kj::mv(stmt)) {}

  friend class SqliteDatabase;
};

// Represents one SQLite query.
//
// Only one Query can exist at a time, for a given database. It should probably be allocated on
// the stack.
class SqliteDatabase::Query {
public:
  using ValuePtr = kj::OneOf<kj::ArrayPtr<const byte>, kj::StringPtr, int64_t, double,
                             decltype(nullptr)>;

  // Construct using Statement::run() or SqliteDatabase::run().

  ~Query() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Query);

  // Row IO counter.
  uint64_t getRowsRead();
  // Row IO counter.
  uint64_t getRowsWritten();

  // If true, there are no more rows. (When true, the methods below must not be called.)
  bool isDone() { return done; }

  // For INSERT, UPDATE, or DELETE queries, returns the number of rows changed. For other query
  // types the result is undefined.
  uint changeCount();

  // Advance to the next row.
  void nextRow();

  // How many columns does each row of the result have?
  uint columnCount();

  // Get the value at the given column, as whatever type was actually returned.
  //
  // Returned pointers (strings and blobs) remain valid only until either (a) nextRow() is called,
  // or (b) a different get method is called on the same column but with a different type.
  ValuePtr getValue(uint column);

  // Get the name of a specific column.
  kj::StringPtr getColumnName(uint column);

  // Get the value at the given column, coercing it to the desired type according to SQLite rules.
  kj::ArrayPtr<const byte> getBlob(uint column);

  // Get the value at the given column, coercing it to the desired type according to SQLite rules.
  kj::StringPtr getText(uint column);

  // Get the value at the given column, coercing it to the desired type according to SQLite rules.
  int getInt(uint column);

  // Get the value at the given column, coercing it to the desired type according to SQLite rules.
  int64_t getInt64(uint column);

  // Get the value at the given column, coercing it to the desired type according to SQLite rules.
  double getDouble(uint column);

  // Get the value at the given column, coercing it to the desired type according to SQLite rules.
  bool isNull(uint column);

  kj::Maybe<kj::ArrayPtr<const byte>> getMaybeBlob(uint column) {
    if (isNull(column)) { return kj::none; } else { return getBlob(column); }
  }
  kj::Maybe<kj::StringPtr> getMaybeText(uint column) {
    if (isNull(column)) { return kj::none; } else { return getText(column); }
  }
  kj::Maybe<int> getMaybeInt(uint column) {
    if (isNull(column)) { return kj::none; } else { return getInt(column); }
  }
  kj::Maybe<int64_t> getMaybeInt64(uint column) {
    if (isNull(column)) { return kj::none; } else { return getInt64(column); }
  }
  kj::Maybe<double> getMaybeDouble(uint column) {
    if (isNull(column)) { return kj::none; } else { return getDouble(column); }
  }

private:
  SqliteDatabase& db;
  Regulator& regulator;
  kj::Own<sqlite3_stmt> ownStatement;   // for one-off queries
  sqlite3_stmt* statement;
  bool done = false;

  friend class SqliteDatabase;

  Query(SqliteDatabase& db, Regulator& regulator, Statement& statement,
        kj::ArrayPtr<const ValuePtr> bindings);
  Query(SqliteDatabase& db, Regulator& regulator, kj::StringPtr sqlCode,
        kj::ArrayPtr<const ValuePtr> bindings);
  template <typename... Params>
  Query(SqliteDatabase& db, Regulator& regulator, Statement& statement, Params&&... bindings)
      : db(db), regulator(regulator), statement(statement) {
    bindAll(std::index_sequence_for<Params...>(), kj::fwd<Params>(bindings)...);
  }
  template <typename... Params>
  Query(SqliteDatabase& db, Regulator& regulator, kj::StringPtr sqlCode, Params&&... bindings)
      : db(db), regulator(regulator),
        ownStatement(db.prepareSql(regulator, sqlCode, 0, MULTI)),
        statement(ownStatement) {
    bindAll(std::index_sequence_for<Params...>(), kj::fwd<Params>(bindings)...);
  }

  void checkRequirements(size_t size);

  void init(kj::ArrayPtr<const ValuePtr> bindings);
  void resetRowCounters();

  void bind(uint column, ValuePtr value);
  void bind(uint column, kj::ArrayPtr<const byte> value);
  void bind(uint column, kj::StringPtr value);
  void bind(uint column, long long value);
  void bind(uint column, double value);
  void bind(uint column, decltype(nullptr));

  // Some reasonable automatic conversions.

  inline void bind(uint column, int value) { bind(column, static_cast<long long>(value)); }
  inline void bind(uint column, uint value) { bind(column, static_cast<long long>(value)); }
  inline void bind(uint column, long value) { bind(column, static_cast<long long>(value)); }
  inline void bind(uint column, float value) { bind(column, static_cast<double>(value)); }

  template <typename... T, size_t... i>
  void bindAll(std::index_sequence<i...>, T&&... value) {
    checkRequirements(sizeof...(T));
    (bind(i, value), ...);
    nextRow();
  }
};

// Options affecting SqliteDatabase::Vfs onstructor.
struct SqliteDatabase::VfsOptions {

  // Value that should be returned by the SQLite VFS's xDeviceCharacteristics method. This is
  // a combination of SQLITE_IOCAP_* flags which can improve performance if the device is known
  // to provide certain guarantees.
  //
  // SQLite's default filesystem driver sets this to 0 on unix. On Windows, it sets the
  // SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN flag. SQLite also lets the application enable
  // SQLITE_IOCAP_POWERSAFE_OVERWRITE explicitly via the SQLITE_FCNTL_POWERSAFE_OVERWRITE file
  // control, or the `?psow=1` URL parameter. It is believed that almost all modern disks support
  // powersafe overwrite, and being able to assume this significantly improves performance.
  // See: https://www.sqlite.org/psow.html Because it's almost always desirable, this
  // implementation enables powersafe overwrite by default.
  //
  // Note that when the underlying directory is a real disk directory, then this implementation
  // will fall back to the native VFS implementation. In that case, the options you set here will
  // be ORed with the ones set by the underlying VFS.
  int deviceCharacteristics = 0x00001000;  // = SQLITE_FCNTL_POWERSAFE_OVERWRITE
};

// Implements a SQLite VFS based on a KJ directory.
//
// If the directory is detected to be a disk directory (i.e. getFd() or getWin32Handle() returns
// non-null), this VFS implementation will actually delegate to the built-in one. This ensures
// feature-parity for production use.
//
// If the directory is not a disk directory, then the VFS will actually use the KJ APIs, but
// some features will be missing. Most importantly, as of this writing, KJ filesystem APIs do
// not support locks, so all locking will be ignored.
//
// An instance of `Vfs` can safely be used across multiple threads.
class SqliteDatabase::Vfs {
public:
  // Pretend `Options` is declared nested here. Due to a C++ quirk, we cannot actually declare it
  // nested while having default-initialized parameters of this type.
  using Options = VfsOptions;

  // Create a VFS backed by the given kj::Directory.
  //
  // If the directory is a real disk directory (i.e. getFd() returns non-null), then this will
  // use SQLite's native filesystem implementation AND locking implementation. This is what you
  // want when opening a database that could simultaneously be opened by other programs which may
  // not be using this wrapper library.
  //
  // If the directory is NOT a real disk directory, this constructor will only arrange to do
  // locking between clients that use the same Vfs object. This makes sense for in-memory temporary
  // filesystems and other cases where the application can ensure all clients are using the same
  // Vfs. If, somehow, the same database file is opened for write via two different `Vfs` instances,
  // it will likely become corrupted.
  explicit Vfs(const kj::Directory& directory, Options options = {});

  // Create a VFS with custom lock management.
  //
  // Unlike the other constructor, this version never uses SQLite's native VFS implementation.
  // `lockManager` will be responsible for coordinating access between multiple concurrent clients
  // of the same database.
  explicit Vfs(const kj::Directory& directory, const LockManager& lockManager,
               Options options = {});

  ~Vfs() noexcept(false);

  // Unfortunately, all SQLite VFSes must be registered in a global list with unique names, and
  // then the _name_ must be passed to sqlite3_open_v2() to use it when opening a database. This is
  // dumb, you should instead be able to simply pass the sqlite3_vfs* when opening the database,
  // but this is the way it is. To work around this, each VFS is assigned an auto-generated unique
  // name.
  //
  // TODO(cleanup): Patch SQLite to allow passing the pointer in?
  kj::StringPtr getName() const { return name; }

  KJ_DISALLOW_COPY_AND_MOVE(Vfs);

private:
  const kj::Directory& directory;
  kj::Own<LockManager> ownLockManager;
  const LockManager& lockManager;
  Options options;

  // Value returned by getName();
  kj::String name = makeName();

  sqlite3_vfs& native;  // the system's default VFS implementation
  kj::Own<sqlite3_vfs> vfs;  // our VFS

  // Result of `directory.getFd()`, if it returns non-null. Cached here for convenience.
  int rootFd = -1;

  template <typename T, T slot>
  struct MethodWrapperHack;

  struct WrappedNativeFileImpl;
  // Create a VFS definition that wraps the native VFS implementation except that it treats our
  // `directory` as the root. Requires that the directory is a real disk directory (and `rootFd`
  // is filled in).
  sqlite3_vfs makeWrappedNativeVfs();

  struct FileImpl;
  // Create a VFS definition that actually delegates to the KJ filesystem.
  sqlite3_vfs makeKjVfs();

  // Create the value returned by `getName()`. Called once at construction time and cached in
  // `name`.
  kj::String makeName();

  // Tries to create a new path by appending the given path to this VFS's root directory path.
  // This allows us to use the system's default VFS implementation, without wrapping, by passing
  // the result of this function to sqlite3_open_v2().
  //
  // Unfortunately, this requires getting a file path from a kj::Directory. On Windows, we can use
  // the GetFinalPathNameByHandleW() API. On Unix, there's no portable way to do this.
  kj::Maybe<kj::Path> tryAppend(kj::PathPtr suffix) const;

  friend class SqliteDatabase;
  class DefaultLockManager;
};

class SqliteDatabase::LockManager {
public:
  // Obtain a lock for the given database path. The main database file is also provided in case
  // it is useful. This method only creates the `Lock` object; it's level starts out as UNLOCKED,
  // meaning no actual lock is held yet.
  //
  // `lock()` is only invoked for main database files. SQLite opens other files (journal, WAL); no
  // `Lock` object is obtained for these.
  //
  // If the same database file is opened multiple times via the same `Vfs`, a separate `Lock`
  // will be obtained each time, so that these locks can coordinate between databases in the
  // same process. Since typically these databases would be in separate threads, the `lock()`
  // method is thread-safe (hence `const`). However, a `Lock` instance itself is only accessed
  // from the calling thread.
  virtual kj::Own<Lock> lock(kj::PathPtr path, const kj::ReadableFile& mainDatabaseFile) const = 0;
};

// Implements file locks and shared memory space used to coordination between clients of a
// particular database. It is expected that if the database is accessible from other processes,
// this object will coordinate with them.
//
// When using a Vfs based on a regular disk directory, this class isn't used; instead, SQLite's
// native implementation kicks in, which is based on advisory file locks at the OS level, as well
// as mmapped shared memory from a file next to the database with suffix `-shm`.
class SqliteDatabase::Lock {
public:
  // The main database can be locked at one of these levels.
  //
  // See the SQLite documentation for an explanation of lock levels:
  //     https://www.sqlite.org/lockingv3.html
  //
  // Note, however, that this locking scheme is mostly unused in WAL mode, which everyone should
  // be using now. In WAL mode, clients almost always have only a `SHARED` lock. It is inreased
  // to `EXCLUSIVE` only when shutting down the database, in order to safely delete the WAL and
  // WAL-index (-shm) files.
  //
  // (The values of this enum correspond to the SQLITE_LOCK_* constants, but we're trying to
  // avoid including sqlite's header here.)
  enum Level {
    UNLOCKED,
    SHARED,
    RESERVED,
    PENDING,
    EXCLUSIVE
  };

  // Increase the lock's level. Returns false if the requested level is not available. This
  // method never blocks; SQLite takes care of retrying if needed. Per SQLite docs, if an attempt
  // to request an EXCLUSIVE lock fails because of other shared locks (but not other exclusive
  // locks), the lock will still have transitioned to the PENDING state, which prevents new shared
  // locks from being taken.
  //
  // The Lock starts an level UNLOCKED.
  virtual bool tryIncreaseLevel(Level level) = 0;

  // Reduce the lock's level. `level` is either UNLOCKED or SHARED.
  virtual void decreaseLevel(Level level) = 0;

  // Check if any client has a RESERVED lock on the database.
  virtual bool checkReservedLock() = 0;

  // Get a shared memory region. All regions have the same size, so `size` will be the same for
  // every call. If `index` exceeds the number of regions that exist so far, and `extend` is false,
  // this returns an empty array, but if `extend` is true, all regions through the given index are
  // created (containing zeros).
  //
  // The returned array is valid until the object is destroyed, or clearSharedMemory() is called.
  virtual kj::ArrayPtr<byte> getSharedMemoryRegion(uint index, uint size, bool extend) = 0;

  // Deletes all shared memory regions.
  //
  // Called when shutting down the last database client or converting away from WAL mode. The
  // caller will obtain an exclusive lock before calling this.
  //
  // The LockManager is also allowed to discard shared memory automatically any time it knows for
  // sure that there are no clients.
  virtual void clearSharedMemory() = 0;

  // Attempt to obtain shared or exclusive locks for the given WAL-mode lock indices, which are in
  // the range [0, WAL_LOCK_COUNT). Returns true if the locks were successfully obtained (for all
  // of them), false if at least one lock wasn't available (in which case no change was made). A
  // shared lock can be obtained as long as there are no exclusive locks. An exclusive lock can be
  // obtained as long as there are no other locks of any kind.
  //
  // The caller may request a shared lock multiple times, in which case it is expected to unlock
  // the same number of times.
  virtual bool tryLockWalShared(uint start, uint count) = 0;

  // Attempt to obtain shared or exclusive locks for the given WAL-mode lock indices, which are in
  // the range [0, WAL_LOCK_COUNT). Returns true if the locks were successfully obtained (for all
  // of them), false if at least one lock wasn't available (in which case no change was made). A
  // shared lock can be obtained as long as there are no exclusive locks. An exclusive lock can be
  // obtained as long as there are no other locks of any kind.
  //
  // The caller may request a shared lock multiple times, in which case it is expected to unlock
  // the same number of times.
  virtual bool tryLockWalExclusive(uint start, uint count) = 0;

  // Release a previously-obtained WAL-mode lock.
  virtual void unlockWalShared(uint start, uint count) = 0;

  // Release a previously-obtained WAL-mode lock.
  virtual void unlockWalExclusive(uint start, uint count) = 0;

  // There are exactly this many WAL-mode locks.
  static constexpr uint WAL_LOCK_COUNT = 8;

  // SQLite sets aside bytes [120, 128) of the first shared memory region for use by the WAL locking
  // implementation. SQLite will never touch these bytes. This may or may not be needed by your
  // implementation. SQLite's native implementation on Windows acquires locks on these specific
  // bytes because Windows file locks are mandatory, meaning they actually block concurrent reads
  // and writes. SQLite really wants "advisory" locks which block other locks but don't actually
  // block reads and writes. So, it applies the mandatory locks to these bytes which are never
  // otherwise read nor written.
  static constexpr uint RESERVED_LOCK_BYTES_OFFSET = 120;
};

template <typename... Params>
SqliteDatabase::Query SqliteDatabase::run(
    Regulator& regulator, kj::StringPtr sqlCode, Params&&... params) {
  return Query(*this, regulator, sqlCode, kj::fwd<Params>(params)...);
}

template <typename... Params>
SqliteDatabase::Query SqliteDatabase::Statement::run(Params&&... params) {
  return Query(db, regulator, *this, kj::fwd<Params>(params)...);
}

template <size_t size>
SqliteDatabase::Statement SqliteDatabase::prepare(const char (&sqlCode)[size]) {
  return prepare(TRUSTED, kj::StringPtr(sqlCode, size - 1));
}
template <size_t size, typename... Params>
SqliteDatabase::Query SqliteDatabase::run(
    const char (&sqlCode)[size], Params&&... params) {
  return Query(*this, TRUSTED, sqlCode, kj::fwd<Params>(params)...);
}

}  // namespace workerd
