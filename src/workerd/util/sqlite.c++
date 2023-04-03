// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite.h"
#include <kj/debug.h>

#if _WIN32
#include <kj/win32-api-version.h>
#include <windows.h>
#include <kj/windows-sanity.h>
#else
#include <unistd.h>
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include <kj/vector.h>
#include <kj/map.h>
#include <kj/mutex.h>
#include <atomic>

namespace workerd {

#define SQLITE_CALL_NODB(code, ...) do { \
    int _ec = code; \
    KJ_ASSERT(_ec == SQLITE_OK, "SQLite failed: " #code, sqlite3_errstr(_ec), ##__VA_ARGS__); \
  } while (false)
// Make a SQLite call and check the returned error code. Use this version when the call is not
// associated with an open DB connection.

#define SQLITE_CALL(code, ...) do { \
    int _ec = code; \
    /* SQLITE_MISUSE doesn't put error info on the database object, so check it separately */ \
    KJ_ASSERT(_ec != SQLITE_MISUSE, "SQLite misused: " #code, ##__VA_ARGS__); \
    KJ_ASSERT(_ec == SQLITE_OK, "SQLite failed: " #code, sqlite3_errmsg(db), ##__VA_ARGS__); \
  } while (false)
// This version requires the scope to contain a variable named `db` which is of type sqlite3*, or
// can convert to it.

#define SQLITE_CALL_FAILED(code, error, ...) do { \
    KJ_ASSERT(error != SQLITE_MISUSE, "SQLite misused: " code, ##__VA_ARGS__); \
    KJ_ASSERT(error == SQLITE_OK, "SQLite failed: " code, sqlite3_errmsg(db), ##__VA_ARGS__); \
  } while (false);
// Version of `SQLITE_CALL` that can be called after inspecting the error code, in case some codes
// aren't really errors.

namespace {

void disposeSqlite(sqlite3_stmt* stmt) {
  sqlite3_finalize(stmt);

  // Note that any returned error code is actually the last error to occur while executing the
  // statement. This does not really mean that finalization failed, and the error in question
  // should have been checked and reported earlier. So, we ignore it here.
}

template <typename T>
class SqliteDisposer: public kj::Disposer {
public:
  void disposeImpl(void* pointer) const override {
    disposeSqlite(reinterpret_cast<T*>(pointer));
  }
};

template <typename T>
kj::Own<T> ownSqlite(T* obj) {
  static const SqliteDisposer<T> disposer;
  return kj::Own<T>(obj, disposer);
}

#if _WIN32
// https://github.com/capnproto/capnproto/blob/master/c%2B%2B/src/kj/filesystem-disk-win32.c%2B%2B#L255-L269
static kj::Path getPathFromWin32Handle(HANDLE handle) {
  DWORD tryLen = MAX_PATH;
  for (;;) {
    auto temp = kj::heapArray<wchar_t>(tryLen + 1);
    DWORD len = GetFinalPathNameByHandleW(handle, temp.begin(), tryLen, 0);
    if (len == 0) {
      KJ_FAIL_WIN32("GetFinalPathNameByHandleW", GetLastError());
    }
    if (len < temp.size()) {
      return kj::Path::parseWin32Api(temp.slice(0, len));
    }
    // Try again with new length.
    tryLen = len;
  }
}
#endif

}  // namespace

// =======================================================================================

SqliteDatabase::SqliteDatabase(const Vfs& vfs, kj::PathPtr path) {
  KJ_IF_MAYBE(rootedPath, vfs.tryAppend(path)) {
    // If we can get the path rooted in the VFS's directory, use the system's default VFS instead
    SQLITE_CALL_NODB(sqlite3_open_v2(rootedPath->toString().cStr(), &db,
                                    SQLITE_OPEN_READONLY, nullptr));
  } else {
    SQLITE_CALL_NODB(sqlite3_open_v2(path.toString().cStr(), &db,
                                    SQLITE_OPEN_READONLY, vfs.getName().cStr()));
  }
}

SqliteDatabase::SqliteDatabase(const Vfs& vfs, kj::PathPtr path, kj::WriteMode mode) {
  int flags = SQLITE_OPEN_READWRITE;
  if (kj::has(mode, kj::WriteMode::CREATE)) {
    flags |= SQLITE_OPEN_CREATE;

    if (kj::has(mode, kj::WriteMode::CREATE_PARENT) && path.size() > 1) {
      // SQLite isn't going to try to create the parent directory so let's try to create it now.
      vfs.directory.openSubdir(path.parent(),
          kj::WriteMode::CREATE | kj::WriteMode::MODIFY | kj::WriteMode::CREATE_PARENT);
    }
  }
  KJ_REQUIRE(kj::has(mode, kj::WriteMode::MODIFY), "SQLite doesn't support create-exclusive mode");

  KJ_IF_MAYBE(rootedPath, vfs.tryAppend(path)) {
    // If we can get the path rooted in the VFS's directory, use the system's default VFS instead
    SQLITE_CALL_NODB(sqlite3_open_v2(rootedPath->toString().cStr(), &db,
                                    flags, nullptr));
  } else {
    SQLITE_CALL_NODB(sqlite3_open_v2(path.toString().cStr(), &db,
                                    flags, vfs.getName().cStr()));
  }
}

SqliteDatabase::~SqliteDatabase() noexcept(false) {
  auto err = sqlite3_close(db);
  if (err == SQLITE_BUSY) {
    KJ_LOG(ERROR, "sqlite database destroyed while dependent objects still exist");
    // SQLite actually provides a lazy-close API which we might as well use here instead of leaking
    // memory.
    err = sqlite3_close_v2(db);
  }

  KJ_REQUIRE(err == SQLITE_OK, sqlite3_errstr(err)) { break; }
}

kj::Own<sqlite3_stmt> SqliteDatabase::prepareSql(sqlite3* db, kj::StringPtr sqlCode, uint prepFlags,
                                                 Multi multi) {
  for (;;) {
    sqlite3_stmt* result;
    const char* tail;

    SQLITE_CALL(sqlite3_prepare_v3(db, sqlCode.begin(), sqlCode.size(), prepFlags, &result, &tail));
    KJ_REQUIRE(result != nullptr, "SQL code did not contain a statement", sqlCode);
    auto ownResult = ownSqlite(result);

    while (*tail == ' ' || *tail == '\n') ++tail;

    switch (multi) {
      case SINGLE:
        KJ_REQUIRE(tail == sqlCode.end(), "sqlite statement had leftover code", tail);
        break;

      case MULTI:
        if (tail != sqlCode.end()) {
          KJ_REQUIRE(sqlite3_bind_parameter_count(result) == 0,
              "only the last statement in a multi-statement query can have parameters");

          // This isn't the last statement in the code. Execute it immediately.
          int err = sqlite3_step(result);
          if (err == SQLITE_DONE) {
            // good
          } else if (err == SQLITE_ROW) {
            // Intermediate statement returned results. We will discard.
          } else {
            SQLITE_CALL_FAILED("sqlite3_step()", err);
          }

          // Reduce `sqlCode` to include only what we haven't already executed.
          sqlCode = kj::StringPtr(tail, sqlCode.end());
          continue;
        }
        break;
    }

    return ownResult;
  }
}

SqliteDatabase::Statement SqliteDatabase::prepare(kj::StringPtr sqlCode) {
  return Statement(*this, prepareSql(db, sqlCode, SQLITE_PREPARE_PERSISTENT, SINGLE));
}

SqliteDatabase::Query::Query(SqliteDatabase& db, Statement& statement,
                             kj::ArrayPtr<const ValuePtr> bindings)
    : db(db), statement(statement) {
  init(bindings);
}

SqliteDatabase::Query::Query(SqliteDatabase& db, kj::StringPtr sqlCode,
                             kj::ArrayPtr<const ValuePtr> bindings)
    : db(db), ownStatement(prepareSql(db, sqlCode, 0, SINGLE)), statement(ownStatement) {
  init(bindings);
}

SqliteDatabase::Query::~Query() noexcept(false) {
  // We only need to reset the statement if we don't own it. If we own it, it's about to be
  // destroyed anyway.
  if (ownStatement.get() == nullptr) {
    // The error code returned by sqlite3_reset() actually represents the last error encountered
    // when stepping the statement. This doesn't mean that the reset failed.
    sqlite3_reset(statement);

    // sqlite3_clear_bindings() returns int, but there is no documentation on how the return code
    // should be interpreted, so we ignore it.
    sqlite3_clear_bindings(statement);
  }
}

void SqliteDatabase::Query::checkRequirements(size_t size) {
  KJ_REQUIRE(!sqlite3_stmt_busy(statement), "only one Query can run at a time");
  KJ_REQUIRE(size == sqlite3_bind_parameter_count(statement),
      "wrong number of bindings for SQLite query");
}

void SqliteDatabase::Query::init(kj::ArrayPtr<const ValuePtr> bindings) {
  checkRequirements(bindings.size());

  for (auto i: kj::indices(bindings)) {
    bind(i, bindings[i]);
  }

  nextRow();
}

void SqliteDatabase::Query::bind(uint i, ValuePtr value) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(blob, kj::ArrayPtr<const byte>) {
      SQLITE_CALL(sqlite3_bind_blob(statement, i+1, blob.begin(), blob.size(), SQLITE_STATIC));
    }
    KJ_CASE_ONEOF(text, kj::StringPtr) {
      SQLITE_CALL(sqlite3_bind_text(statement, i+1, text.begin(), text.size(), SQLITE_STATIC));
    }
    KJ_CASE_ONEOF(n, int64_t) {
      SQLITE_CALL(sqlite3_bind_int64(statement, i+1, static_cast<long long>(n)));
    }
    KJ_CASE_ONEOF(x, double) {
      SQLITE_CALL(sqlite3_bind_double(statement, i+1, x));
    }
    KJ_CASE_ONEOF(_, decltype(nullptr)) {
      SQLITE_CALL(sqlite3_bind_null(statement, i+1));
    }
  }
}

void SqliteDatabase::Query::bind(uint i, kj::ArrayPtr<const byte> value) {
  SQLITE_CALL(sqlite3_bind_blob(statement, i+1, value.begin(), value.size(), SQLITE_STATIC));
}

void SqliteDatabase::Query::bind(uint i, kj::StringPtr value) {
  SQLITE_CALL(sqlite3_bind_text(statement, i+1, value.begin(), value.size(), SQLITE_STATIC));
}

void SqliteDatabase::Query::bind(uint i, long long value) {
  SQLITE_CALL(sqlite3_bind_int64(statement, i+1, value));
}

void SqliteDatabase::Query::bind(uint i, double value) {
  SQLITE_CALL(sqlite3_bind_double(statement, i+1, value));
}

void SqliteDatabase::Query::bind(uint i, decltype(nullptr)) {
  SQLITE_CALL(sqlite3_bind_null(statement, i+1));
}

void SqliteDatabase::Query::nextRow() {
  int err = sqlite3_step(statement);
  if (err == SQLITE_DONE) {
    done = true;
  } else if (err != SQLITE_ROW) {
    SQLITE_CALL_FAILED("sqlite3_step()", err);
  }
}

uint SqliteDatabase::Query::changeCount() {
  KJ_REQUIRE(done);
  KJ_DREQUIRE(columnCount() == 0,
      "changeCount() can only be called on INSERT/UPDATE/DELETE queries");
  return sqlite3_changes(db);
}

uint SqliteDatabase::Query::columnCount() {
  return sqlite3_column_count(statement);
}

SqliteDatabase::Query::ValuePtr SqliteDatabase::Query::getValue(uint column) {
  switch (sqlite3_column_type(statement, column)) {
    case SQLITE_INTEGER: return getInt64(column);
    case SQLITE_FLOAT:   return getDouble(column);
    case SQLITE_TEXT:    return getText(column);
    case SQLITE_BLOB:    return getBlob(column);
    case SQLITE_NULL:    return nullptr;
  }
  KJ_UNREACHABLE;
}

kj::ArrayPtr<const byte> SqliteDatabase::Query::getBlob(uint column) {
  const byte* ptr = reinterpret_cast<const byte*>(sqlite3_column_blob(statement, column));
  return kj::arrayPtr(ptr, sqlite3_column_bytes(statement, column));
}

kj::StringPtr SqliteDatabase::Query::getText(uint column) {
  const char* ptr = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
  return kj::StringPtr(ptr, sqlite3_column_bytes(statement, column));
}

int SqliteDatabase::Query::getInt(uint column) {
  return sqlite3_column_int(statement, column);
}

int64_t SqliteDatabase::Query::getInt64(uint column) {
  return sqlite3_column_int64(statement, column);
}

double SqliteDatabase::Query::getDouble(uint column) {
  return sqlite3_column_double(statement, column);
}

bool SqliteDatabase::Query::isNull(uint column) {
  return sqlite3_column_type(statement, column) == SQLITE_NULL;
}

// =======================================================================================
// VFS

// -----------------------------------------------------------------------------
// Code to wrap SQLite's native VFS so that it can be rooted in some `kj::Directory`, where that
// directory points at a real disk directory.
//
// A native disk `kj::Directory` -- at least on Unix -- wraps an open file descriptor, pointing at
// a directory. It does NOT keep track of the directory's path on disk. In fact, the directory can
// be moved or renamed, and `kj::Directory` will continue to point at it.
//
// There is no portable way to query the current path of a directory. In order to open files within
// a directory given only the directory descriptor, you must use syscalls like `openat()`, which
// take a directory file descriptor to use as the root.
//
// SQLite's native VFS, however, is not openat()-aware. Luckily, it _does_ provide the ability to
// redirect its syscalls to custom implementations. So we can intercept `open()` and make it use
// `openat()` instead! With a little thread-local hackery, we can make sure to use the desired root
// directory descriptor from the `kj::Directory`.
//
// Of course, SQLite also lets us virtualize the whole filesystem at a higher level. Why go to all
// the bother to hack it at a low level rather than just implement an entire VFS based on the
// `kj::Directory` interface? The problem is, SQLite's native VFS contains a ton of code to handle
// all sorts of corner cases and do things just right. When our files are actually on real disk,
// we want to leverage all that code. If we can just make it interpret paths differently, then we
// can reuse the rest of the implementation.

#if !_WIN32
namespace {

static thread_local int currentVfsRoot = AT_FDCWD;
// We will tell SQLite to use alternate implementations of path-oriented syscalls which use the
// `*at()` versions of the calls with `currentVfsRoot` as the directory descriptor. When the
// descriptor is `AT_FDCWD`, this will naturally reproduce the behavior of the non-`at()` versions.
// We temporarily swap this for a real desriptor when our custom VFS wrapper is being invoked.

static int replaced_open(const char* path, int flags, int mode) {
  return openat(currentVfsRoot, path, flags, mode);
}
static int replaced_access(const char* path, int type) {
  return faccessat(currentVfsRoot, path, type, 0);
}
static char* replaced_getcwd(char* buf, size_t size) noexcept {
  KJ_REQUIRE(currentVfsRoot == AT_FDCWD,
      "SQLite custom VFS shouldn't call getcwd() because we overrode xFullPathname");
  return getcwd(buf, size);
}
static int replaced_stat(const char* path, struct stat* stats) {
  return fstatat(currentVfsRoot, path, stats, 0);
}
static int replaced_unlink(const char* path) {
  return unlinkat(currentVfsRoot, path, 0);
}
static int replaced_mkdir(const char* path, mode_t mode) {
  return mkdirat(currentVfsRoot, path, mode);
}
static int replaced_rmdir(const char* path) {
  return unlinkat(currentVfsRoot, path, AT_REMOVEDIR);
}
static ssize_t replaced_readlink(const char* path, char* buf, size_t len) {
  return readlinkat(currentVfsRoot, path, buf, len);
}
static int replaced_lstat(const char* path, struct stat* stats) {
  return fstatat(currentVfsRoot, path, stats, AT_SYMLINK_NOFOLLOW);
}

};

struct SqliteDatabase::Vfs::WrappedNativeFileImpl: public sqlite3_file {
  // The sqlite3_file implementation we use when wrapping the native filesystem.

  int rootFd;

  // It's expected that the wrapped sqlite_file begins in memory immediately after this object.
  sqlite3_file* getWrapped() { return reinterpret_cast<sqlite3_file*>(this + 1); }

  static const sqlite3_io_methods METHOD_TABLE;
};

template <typename Result, typename... Params,
          Result (*sqlite3_vfs::*slot)(sqlite3_vfs* vfs, Params...)>
struct SqliteDatabase::Vfs::MethodWrapperHack<
    Result (*sqlite3_vfs::*)(sqlite3_vfs* vfs, Params...), slot> {
  // This completely nutso template generates wrapper functions for each of the function pointer
  // members of sqlite3_vfs. The wrapper function temporarily sets `currentVfsRoot` to the FD
  // of the directory from the SqliteDatabase::Vfs instance in use, then invokes the same function
  // on the underlying native VFS.

  static Result wrapper(sqlite3_vfs* vfs, Params... params) noexcept {
    auto& self = *reinterpret_cast<SqliteDatabase::Vfs*>(vfs->pAppData);
    KJ_ASSERT(currentVfsRoot == AT_FDCWD);
    currentVfsRoot = self.rootFd;
    KJ_DEFER(currentVfsRoot = AT_FDCWD);
    return (self.native.*slot)(&self.native, params...);
  }
};

template <typename Result, typename... Params,
          Result (*sqlite3_io_methods::*slot)(sqlite3_file* file, Params...)>
struct SqliteDatabase::Vfs::MethodWrapperHack<
    Result (*sqlite3_io_methods::*)(sqlite3_file* file, Params...), slot> {
  // Specialization of MethodWrapperHack for wrapping methods of sqlite_file, aka
  // sqlite3_io_methods. Unfortunately, some file methods go back and perform filesystem ops. In
  // particular, accessing shared memory associated with a file actually opens another adjacent
  //file.

  static Result wrapper(sqlite3_file* file, Params... params) noexcept {
    auto wrapper = static_cast<WrappedNativeFileImpl*>(file);
    file = wrapper->getWrapped();
    KJ_ASSERT(currentVfsRoot == AT_FDCWD);
    currentVfsRoot = wrapper->rootFd;
    KJ_DEFER(currentVfsRoot = AT_FDCWD);
    return (file->pMethods->*slot)(file, params...);
  }
};

const sqlite3_io_methods SqliteDatabase::Vfs::WrappedNativeFileImpl::METHOD_TABLE = {
  .iVersion = 3,

#define WRAP(name) \
  .name = &MethodWrapperHack<decltype(&sqlite3_io_methods::name), \
                             &sqlite3_io_methods::name>::wrapper

  WRAP(xClose),
  WRAP(xRead),
  WRAP(xWrite),
  WRAP(xTruncate),
  WRAP(xSync),
  WRAP(xFileSize),
  WRAP(xLock),
  WRAP(xUnlock),
  WRAP(xCheckReservedLock),
  WRAP(xFileControl),
  WRAP(xSectorSize),
  WRAP(xDeviceCharacteristics),

  WRAP(xShmMap),
  WRAP(xShmLock),
  WRAP(xShmBarrier),
  WRAP(xShmUnmap),

  WRAP(xFetch),
  WRAP(xUnfetch),
#undef WRAP
};

sqlite3_vfs SqliteDatabase::Vfs::makeWrappedNativeVfs() {
  // The native VFS gives us the ability to override its syscalls. We need to do so, in
  // particular to force them to use the *at() versions of the calls that accept a directory FD
  // to use as the root.
  //
  // Unfortunately, these overrides are global for the process, with no ability to pass down any
  // context to them. So, we stash the current root FD in `currentVfsRoot` whenever we call into
  // the native VFS. We also don't want to interfere with anything else in the process that is
  // using SQLite directly, so we make sure that when we're not specifically trying to invoke
  // our wrapper, then `currentVfsRoot` is `AT_FDCWD`, which causes the *at() syscalls to match
  // their non-at() versions.
  static bool registerOnce KJ_UNUSED = ([&]() {
#define REPLACE_SYSCALL(name) \
    native.xSetSystemCall(&native, #name, (sqlite3_syscall_ptr)replaced_##name);

    REPLACE_SYSCALL(open);
    REPLACE_SYSCALL(access);
    REPLACE_SYSCALL(getcwd);
    REPLACE_SYSCALL(stat);
    REPLACE_SYSCALL(unlink);
    REPLACE_SYSCALL(mkdir);
    REPLACE_SYSCALL(rmdir);
    REPLACE_SYSCALL(readlink);
    REPLACE_SYSCALL(lstat);
#undef REPLACE_SYSCALL

    return true;
  })();

  // We construct a sqlite3_vfs that is basically a copy of the native VFS, except each method is
  // wrapped so that it sets `currentVfsRoot` while running.
  return {
    .iVersion = kj::min(3, native.iVersion),
    .szOsFile = native.szOsFile + (int)sizeof(WrappedNativeFileImpl),
    .mxPathname = native.mxPathname,
    .pNext = nullptr,
    .zName = name.cStr(),
    .pAppData = this,

    .xOpen = [](sqlite3_vfs* vfs, sqlite3_filename zName, sqlite3_file* file,
                int flags, int *pOutFlags) -> int {
      // We have to wrap xOpen explicitly because we need to furthder wrap each created file.
      //
      // My trick here is to prefix the native file with a second vtable. So the layout of the
      // `sqlite3_file` that we construct is actually a simple `struct sqlite_file` (which just
      // contains a single pointer to sqlite3_io_methods, i.e. the vtable pointer) _followed by_
      // the regular native file structure.

      auto wrapper = static_cast<WrappedNativeFileImpl*>(file);
      file = wrapper->getWrapped();
      file->pMethods = nullptr;

      // Set up currentVfsRoot.
      auto& self = *reinterpret_cast<SqliteDatabase::Vfs*>(vfs->pAppData);
      KJ_ASSERT(currentVfsRoot == AT_FDCWD);
      currentVfsRoot = self.rootFd;
      KJ_DEFER(currentVfsRoot = AT_FDCWD);

      int result = self.native.xOpen(&self.native, zName, file, flags, pOutFlags);

      // `xOpen` setting `pMethods` to non-null indicates that `xClose` is needed, i.e. the file
      // has been constructed. We need our wrapper to match.
      if (file->pMethods == nullptr) {
        wrapper->pMethods = nullptr;
      } else {
        wrapper->pMethods = &WrappedNativeFileImpl::METHOD_TABLE;
        wrapper->rootFd = self.rootFd;
      }

      return result;
    },

#define WRAP(name) \
    .name = &MethodWrapperHack<decltype(&sqlite3_vfs::name), &sqlite3_vfs::name>::wrapper

    WRAP(xDelete),
    WRAP(xAccess),
    .xFullPathname = [](sqlite3_vfs*, const char *zName, int nOut, char *zOut) -> int {
      // Override xFullPathname so that it doesn't rewrite the path at all.
      size_t len = kj::min(strlen(zName), nOut - 1);
      memcpy(zOut, zName, len);
      zOut[len] = 0;
      return SQLITE_OK;
    },

    .xDlOpen = nullptr,
    .xDlError = nullptr,
    .xDlSym = nullptr,
    .xDlClose = nullptr,
    // There is no dlopenat(), but we don't need to support these anyway.

    WRAP(xRandomness),
    WRAP(xSleep),
    WRAP(xCurrentTime),
    WRAP(xGetLastError),
    WRAP(xCurrentTimeInt64),

    .xSetSystemCall = nullptr,
    .xGetSystemCall = nullptr,
    .xNextSystemCall = nullptr,
    // We don't support further overriding syscalls.
#undef WRAP
  };
}
#endif // #if !_WIN32

// -----------------------------------------------------------------------------
// Code to implement a true SQLite VFS based on `kj::Directory`.
//
// This VFS implementation actually delegates to the KJ filesystem interface for everything.
// This is used only when given a `kj::Directory` that does NOT represent a native file, i.e.
// one where `getFd()` returns null. This is mainly used for unit tests which want to use in-memory
// directories.

struct SqliteDatabase::Vfs::FileImpl: public sqlite3_file {
  // Implementation of sqlite3_file.
  //
  // Weirdly, for sqlite3_file, SQLite uses a C++-like inheritance approach, with a separate
  // virtual table that can be shared among all files of the same type. This is different from the
  // way sqlite3_vfs works, where the function pointers are inlined into the sqlite3_vfs struct.
  // In any case, as a result, `FileImpl`, unlike `VfsImpl`, is NOT just a namespace struct, but
  // an actual instance.

  kj::Maybe<const kj::File&> writableFile;
  kj::Own<const kj::ReadableFile> file;

  kj::Maybe<kj::Own<Lock>> lock;
  // Rather complicatedly, SQLite doesn't consider the -shm file to be a separate file that it
  // opens via the VFS, but rather a facet of the database file itself. We implement it using an
  // entirely different interface anyawy.
  //
  // We leave this null if the file is not the main database file.

  FileImpl(kj::Own<const kj::File> file, kj::Maybe<kj::Own<Lock>> lock)
      : sqlite3_file { .pMethods = &FILE_METHOD_TABLE },
        writableFile(*file),
        file(kj::mv(file)),
        lock(kj::mv(lock)) {}
  FileImpl(kj::Own<const kj::ReadableFile> file, kj::Maybe<kj::Own<Lock>> lock)
      : sqlite3_file { .pMethods = &FILE_METHOD_TABLE },
        file(kj::mv(file)),
        lock(kj::mv(lock)) {}

  static const sqlite3_io_methods FILE_METHOD_TABLE;
};

const sqlite3_io_methods SqliteDatabase::Vfs::FileImpl::FILE_METHOD_TABLE = {
  .iVersion = 3,
#define WRAP_METHOD(errorCode, block) \
  auto& self KJ_UNUSED = *static_cast<FileImpl*>(file); \
  try block catch (kj::Exception& e) { \
    KJ_LOG(ERROR, "SQLite VFS I/O error", e); \
    return errorCode; \
  }

  .xClose = [](sqlite3_file* file) noexcept -> int {
    WRAP_METHOD(SQLITE_OK, {
      auto& self = *static_cast<FileImpl*>(file);

      // Caller will free the object's memory, but knows nothing of destructors.
      kj::dtor(self);

      return SQLITE_OK;  // return value is ignored by SQLite
    });
  },

  .xRead = [](sqlite3_file* file, void* buffer, int iAmt, sqlite3_int64 iOfst) noexcept -> int {
    WRAP_METHOD(SQLITE_IOERR_READ, {
      auto bytes = kj::arrayPtr(reinterpret_cast<byte*>(buffer), iAmt);
      size_t actual = self.file->read(iOfst, bytes);

      if (actual < iAmt) {
        memset(bytes.begin() + actual, 0, iAmt - actual);
        return SQLITE_IOERR_SHORT_READ;
      } else {
        return SQLITE_OK;
      }
    });
  },

  .xWrite = [](sqlite3_file* file, const void* buffer, int iAmt,
               sqlite3_int64 iOfst) noexcept -> int {
    WRAP_METHOD(SQLITE_IOERR_WRITE, {
      auto bytes = kj::arrayPtr(reinterpret_cast<const byte*>(buffer), iAmt);
      KJ_REQUIRE_NONNULL(self.writableFile).write(iOfst, bytes);
      return SQLITE_OK;
    });
  },

  .xTruncate = [](sqlite3_file* file, sqlite3_int64 size) noexcept -> int {
    WRAP_METHOD(SQLITE_IOERR_TRUNCATE, {
      KJ_REQUIRE_NONNULL(self.writableFile).truncate(size);
      return SQLITE_OK;
    });
  },

  .xSync = [](sqlite3_file* file, int flags) noexcept -> int {
    WRAP_METHOD(SQLITE_IOERR_FSYNC, {
      if (flags&SQLITE_SYNC_DATAONLY) {
        self.file->datasync();
      } else {
        self.file->sync();
      }
      return SQLITE_OK;
    });
  },

  .xFileSize = [](sqlite3_file* file, sqlite3_int64 *pSize) noexcept -> int {
    WRAP_METHOD(SQLITE_IOERR_FSTAT, {
      *pSize = self.file->stat().size;
      return SQLITE_OK;
    });
  },

  .xLock = [](sqlite3_file* file, int level) noexcept -> int {
    // Verify that our enum's values match the SQLite constants. (We didn't want to include
    // sqlite3.h in our header, so defined a parallel enum.)
    static_assert(Lock::UNLOCKED == SQLITE_LOCK_NONE);
    static_assert(Lock::SHARED == SQLITE_LOCK_SHARED);
    static_assert(Lock::RESERVED == SQLITE_LOCK_RESERVED);
    static_assert(Lock::PENDING == SQLITE_LOCK_PENDING);
    static_assert(Lock::EXCLUSIVE == SQLITE_LOCK_EXCLUSIVE);

    WRAP_METHOD(SQLITE_IOERR_LOCK, {
      auto& lock = *KJ_ASSERT_NONNULL(self.lock,
          "xLock called on file that isn't main database?");
      if (lock.tryIncreaseLevel(static_cast<Lock::Level>(level))) {
        return SQLITE_OK;
      } else {
        return SQLITE_BUSY;
      }
    });
  },

  .xUnlock = [](sqlite3_file* file, int level) noexcept -> int {
    WRAP_METHOD(SQLITE_IOERR_UNLOCK, {
      auto& lock = *KJ_ASSERT_NONNULL(self.lock,
          "xLock called on file that isn't main database?");
      lock.decreaseLevel(static_cast<Lock::Level>(level));
      return SQLITE_OK;
    });
  },

  .xCheckReservedLock = [](sqlite3_file* file, int *pResOut) noexcept -> int {
    WRAP_METHOD(SQLITE_IOERR_CHECKRESERVEDLOCK, {
      auto& lock = *KJ_ASSERT_NONNULL(self.lock,
          "xLock called on file that isn't main database?");
      *pResOut = lock.checkReservedLock();
      return SQLITE_OK;
    });
  },

  .xFileControl = [](sqlite3_file* file, int op, void *pArg) noexcept -> int {
    // Apparently we can return SQLITE_NOTFOUND for controls we don't implement.
    return SQLITE_NOTFOUND;
  },
  .xSectorSize = [](sqlite3_file* file) noexcept -> int {
    // This function doesn't return a status code, it returns the size. It's largely a performance
    // hint, I think. For in-memory file systems, it has no real meaning. 4096 is the value of
    // SQLITE_DEFAULT_SECTOR_SIZE in the SQLite codebase, though the comments also say the result
    // is "almost always 512".
    return 4096;
  },
  .xDeviceCharacteristics = [](sqlite3_file* file) noexcept -> int {
    // This returns a bitfield of SQLITE_IOCAP_* flags that make promises to SQLite, presumably
    // allowing it to perform better. These promises depend on the underlying filesystem details.
    // We can't really make any promises since we don't know anything here. Weirdly enough, the
    // standard unix implementation doesn't seem to set any bits at all, unless you're using f2fs
    // on Linux, or you're using QNX. I guess a lot of this stuff is various guarantees that
    // embedded device vendors implemented in hardware which allow sqlite to run faster on their
    // device.
    return 0;
  },

  .xShmMap = [](sqlite3_file* file, int iRegion, int szRegion, int bExtend,
                void volatile** pp) noexcept -> int {
    WRAP_METHOD(SQLITE_IOERR_SHMMAP, {
      KJ_ASSERT(iRegion >= 0);
      KJ_ASSERT(szRegion >= 0);
      auto& lock = *KJ_ASSERT_NONNULL(self.lock,
          "xShmMap called on file that isn't main database?");

      auto bytes = lock.getSharedMemoryRegion(iRegion, szRegion, bExtend);
      if (bytes == nullptr) {
        *pp = nullptr;
      } else {
        *pp = bytes.begin();
      }
      return SQLITE_OK;
    });
  },
  .xShmLock = [](sqlite3_file* file, int offset, int n, int flags) noexcept -> int {
    WRAP_METHOD(SQLITE_IOERR_SHMLOCK, {
      auto& lock = *KJ_ASSERT_NONNULL(self.lock,
          "xShmMap called on file that isn't main database?");
      if (flags & SQLITE_SHM_LOCK) {
        if (flags & SQLITE_SHM_EXCLUSIVE) {
          if (!lock.tryLockWalExclusive(offset, n)) return SQLITE_BUSY;
        } else {
          KJ_ASSERT(flags & SQLITE_SHM_SHARED);
          if (!lock.tryLockWalShared(offset, n)) return SQLITE_BUSY;
        }
      } else {
        KJ_ASSERT(flags & SQLITE_SHM_UNLOCK);
        if (flags & SQLITE_SHM_EXCLUSIVE) {
          lock.unlockWalExclusive(offset, n);
        } else {
          KJ_ASSERT(flags & SQLITE_SHM_SHARED);
          lock.unlockWalShared(offset, n);
        }
      }
      return SQLITE_OK;
    });
  },
  .xShmBarrier = [](sqlite3_file*) noexcept -> void {
    // I don't quite get why this is virtualized. The native implementation does
    // __sync_synchronize() (equivalent to below, I think) and also "for redundancy" locks and
    // unlocks a mutex.
    std::atomic_thread_fence(std::memory_order_acq_rel);
  },
  .xShmUnmap = [](sqlite3_file* file, int deleteFlag) noexcept -> int {
    WRAP_METHOD(SQLITE_OK, {
      auto& lock = *KJ_ASSERT_NONNULL(self.lock,
          "xShmMap called on file that isn't main database?");
      if (deleteFlag) {
        lock.clearSharedMemory();
      }
      return SQLITE_OK;  // return value is ignored by sqlite
    });
  },

  .xFetch = [](sqlite3_file* file, sqlite3_int64 iOfst, int iAmt, void **pp) noexcept -> int {
    // This is essentially requesting an mmap(). kj::File supports mmap(). Great, right?
    //
    // Well, there's a problem. We mostly use this VFS implementation to wrap an in-memory
    // `kj::File`. Such files support mmap by returning a pointer into the backing store. But
    // while such a mapping exists, the backing store cannot be resized. So write()s that extend
    // the file may fail. This does not work for SQLite's use case.
    //
    // So, alas, we must act like we don't support this. Luckily, SQLite has fallbacks for this.
    *pp = nullptr;
    return SQLITE_OK;
  },
  .xUnfetch = [](sqlite3_file* file, sqlite3_int64 iOfst, void *p) noexcept -> int {
    // Shouldn't ever be called since xFetch() always produces null? But the native implementation
    // return SQLITE_OK even when mmap is disabled so we will too.
    return SQLITE_OK;
  },
#undef WRAP_METHOD
};

sqlite3_vfs SqliteDatabase::Vfs::makeKjVfs() {
  // SQLite VFS implementation based on abstract `kj::Directory`. This is used only when the
  // directory is NOT a true disk directory.
  //
  // This is a namespace-struct defining static methods to fill in the function pointers of
  // sqlite3_vfs.

  return {
    .iVersion = kj::min(3, native.iVersion),
    .szOsFile = sizeof(FileImpl),

    .mxPathname = 512,
    // We have no real limit on paths but SQLite likes to allocate buffers of this size whenever
    // doing path stuff so making it huge would be bad. The default unix implementation uses
    // 512 as a limit so that "should be enough for anyone".

    .pNext = nullptr,
    .zName = name.cStr(),
    .pAppData = this,

#define WRAP_METHOD(errorCode, block) \
      auto& self KJ_UNUSED = *static_cast<SqliteDatabase::Vfs*>(vfs->pAppData); \
      try block catch (kj::Exception& e) { \
        KJ_LOG(ERROR, "SQLite VFS I/O error", e); \
        return errorCode; \
      }

    .xOpen = [](sqlite3_vfs* vfs, sqlite3_filename zName, sqlite3_file* file,
                int flags, int *pOutFlags) -> int {
      WRAP_METHOD(SQLITE_CANTOPEN, {
        auto& target = *static_cast<FileImpl*>(file);

        if (flags & SQLITE_OPEN_READONLY) {
          KJ_REQUIRE(zName != nullptr, "readonly unnamed temporary file? what?");
          KJ_REQUIRE(!(flags & SQLITE_OPEN_CREATE), "create readonly file? what?");

          auto path = kj::Path::parse(zName);
          auto kjFile = KJ_UNWRAP_OR(self.directory.tryOpenFile(path), {
            return SQLITE_CANTOPEN;
          });
          kj::Maybe<kj::Own<Lock>> lock;
          if (flags & SQLITE_OPEN_MAIN_DB) {
            lock = self.lockManager.lock(path, *kjFile);
          }

          kj::ctor(target, kj::mv(kjFile), kj::mv(lock));
        } else {
          kj::Own<const kj::File> kjFile;
          kj::Maybe<kj::Own<Lock>> lock;

          if (zName == nullptr) {
            // Open a temp file.
            KJ_ASSERT(flags & SQLITE_OPEN_DELETEONCLOSE);
            KJ_ASSERT(!(flags & SQLITE_OPEN_MAIN_DB), "main DB can't be a temporary file");
            kjFile = self.directory.createTemporary();
          } else {
            kj::WriteMode mode;
            if (flags & SQLITE_OPEN_CREATE) {
              if (flags & SQLITE_OPEN_EXCLUSIVE) {
                mode = kj::WriteMode::CREATE;
              } else {
                mode = kj::WriteMode::CREATE | kj::WriteMode::MODIFY;
              }
            } else {
              mode = kj::WriteMode::MODIFY;
            }

            auto path = kj::Path::parse(zName);
            kjFile = KJ_UNWRAP_OR(self.directory.tryOpenFile(path, mode), {
              return SQLITE_CANTOPEN;
            });
            if (flags & SQLITE_OPEN_MAIN_DB) {
              lock = self.lockManager.lock(path, *kjFile);
            }

            if (flags & SQLITE_OPEN_DELETEONCLOSE) {
              self.directory.remove(path);
            }
          }

          kj::ctor(target, kj::mv(kjFile), kj::mv(lock));
        }

        // In theory if read-write was requested, but failed, we should retry read-only, and then
        // alter the pOutFlags to reflect this... I'm not going to bother.
        if (pOutFlags != nullptr) {
          *pOutFlags = flags;
        }

        return SQLITE_OK;
      });
    },
    .xDelete = [](sqlite3_vfs* vfs, const char *zName, int syncDir) -> int {
      WRAP_METHOD(SQLITE_IOERR_DELETE, {
        if (self.directory.tryRemove(kj::Path::parse(zName))) {
          return SQLITE_OK;
        } else {
          return SQLITE_IOERR_DELETE_NOENT;
        }
      });
    },
    .xAccess = [](sqlite3_vfs* vfs, const char *zName, int flags, int *pResOut) -> int {
      WRAP_METHOD(SQLITE_IOERR_ACCESS, {
        // Technically, depending on the flags, this may be checking whether the file is readable
        // or writable, rather than just whether it exists. However, the KJ filesystem API
        // assumes that all descendents of a writable directory are readable and writable, hence
        // this is equivalent to checking for existence.
        //
        // If we were to extend the VFS so it can wrap `kj::ReadableDirectory` then in that case
        // we would want to return false when querying writability.
        *pResOut = self.directory.exists(kj::Path::parse(zName));
        return SQLITE_OK;
      });
    },
    .xFullPathname = [](sqlite3_vfs*, const char *zName, int nOut, char *zOut) -> int {
      // Don't rewrite the path at all. All paths are canonical. Our path parsing will reject
      // the existence of `.` or `..` as path components as well as leading `/`.
      size_t len = kj::min(strlen(zName), nOut - 1);
      memcpy(zOut, zName, len);
      zOut[len] = 0;
      return SQLITE_OK;
    },

    .xDlOpen = nullptr,
    .xDlError = nullptr,
    .xDlSym = nullptr,
    .xDlClose = nullptr,
    // We don't support loading shared libraries from virtual files.

    .xRandomness = native.xRandomness,
    .xSleep = native.xSleep,
    .xCurrentTime = native.xCurrentTime,
    .xGetLastError = nullptr,
    .xCurrentTimeInt64 = native.xCurrentTimeInt64,
    // Use native implementations of these OS functions. I'm not sure why these are even part
    // of the VFS. (Exception: xGetLastError is actually sensibly a VFS thing, but we are allowed
    // to just not implement it.)

    .xSetSystemCall = nullptr,
    .xGetSystemCall = nullptr,
    .xNextSystemCall = nullptr,
    // We don't support overriding any syscalls.

#undef WRAP_METHOD
  };
};

// -----------------------------------------------------------------------------

class SqliteDatabase::Vfs::DefaultLockManager final
    : public SqliteDatabase::LockManager {
public:
  kj::Own<Lock> lock(kj::PathPtr path, const kj::ReadableFile& mainDatabaseFile) const override {
    return kj::heap<LockImpl>(*this, path);
  }

private:
  class LockImpl;
  struct LockState;
  using LockMap = kj::HashMap<kj::PathPtr, LockState*>;
  kj::MutexGuarded<LockMap> lockMap;

  struct LockState: public kj::Refcounted {
    // Note: The refcount of this object is protected by `lockMap`'s mutex.

    struct Guarded {
      kj::Vector<kj::Array<byte>> regions;

      uint sharedLockCount = 0;
      bool hasReserved = false;
      bool hasPendingOrExclusive = false;

      uint walLocks[Lock::WAL_LOCK_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0};
      // Each slot contains the count of shared locks, or kj::maxValue if an exclusive lock is
      // held.
    };

    const kj::Path path;
    kj::MutexGuarded<Guarded> guarded;

    LockState(kj::Path path): path(kj::mv(path)) {}
  };

  class LockImpl final: public Lock {
  public:
    LockImpl(const DefaultLockManager& lockManager, kj::PathPtr path)
        : lockManager(lockManager) {
      auto mlock = lockManager.lockMap.lockExclusive();
      auto& slot = mlock->findOrCreate(path, [&]() {
        state = kj::refcounted<LockState>(path.clone());
        return LockMap::Entry {
          .key = state->path,
          .value = state.get()
        };
      });
      if (state.get() == nullptr) {
        state = kj::addRef(*slot);
      }
    }

    ~LockImpl() noexcept(false) {
      // It's important that we drop the state object under lock to ensure no other thread is
      // in the process of grabbing it out of the map at the same time. Since we have to take a
      // lock here anyway, `LockState` uses regular non-atomic refcounts rather than atomic.
      auto mlock = lockManager.lockMap.lockExclusive();
      auto stateToDrop = kj::mv(state);
      if (!stateToDrop->isShared()) {
        mlock->erase(stateToDrop->path);
      }
    }

    bool tryIncreaseLevel(Level newLevel) override {
      if (newLevel <= currentLevel) return true;

      auto slock = state->guarded.lockExclusive();

      if (currentLevel < SHARED) {
        if (slock->hasPendingOrExclusive) {
          return false;
        }
        ++slock->sharedLockCount;
        currentLevel = SHARED;
      }

      if (newLevel == SHARED) {
        return true;
      }

      if (newLevel == RESERVED) {
        if (slock->hasReserved || slock->hasPendingOrExclusive) {
          return false;
        }
        if (currentLevel == SHARED) {
          KJ_ASSERT(slock->sharedLockCount > 0);
          --slock->sharedLockCount;
        }
        slock->hasReserved = true;
        currentLevel = RESERVED;
        return true;
      }

      // Requesting PENDING or EXCLUSIVE. If EXCLUSIVE, we still have to transition through
      // PENDING first, if we're not there already.
      if (currentLevel < PENDING) {
        if (currentLevel != RESERVED && slock->hasReserved) {
          return false;
        }
        if (slock->hasPendingOrExclusive) {
          return false;
        }
        if (currentLevel == SHARED) {
          KJ_ASSERT(slock->sharedLockCount > 0);
          --slock->sharedLockCount;
        }
        slock->hasReserved = false;
        slock->hasPendingOrExclusive = true;
        currentLevel = PENDING;
      }

      if (newLevel == EXCLUSIVE) {
        if (slock->sharedLockCount > 0) {
          return false;
        }
        currentLevel = EXCLUSIVE;
      }

      return true;
    }

    void decreaseLevel(Level newLevel) override {
      if (newLevel >= currentLevel) return;
      KJ_REQUIRE(newLevel <= SHARED);

      auto slock = state->guarded.lockExclusive();
      if (currentLevel >= PENDING) {
        slock->hasPendingOrExclusive = false;
      }
      if (currentLevel == RESERVED) {
        slock->hasReserved = false;
      }
      if (currentLevel == SHARED && newLevel == UNLOCKED) {
        KJ_ASSERT(slock->sharedLockCount > 0);
        --slock->sharedLockCount;
      }
      if (newLevel == SHARED) {
        ++slock->sharedLockCount;
      }
      currentLevel = newLevel;
    }

    bool checkReservedLock() override {
      return state->guarded.lockShared()->hasReserved;
    }

    kj::ArrayPtr<byte> getSharedMemoryRegion(uint index, uint size, bool extend) override {
      if (extend) {
        auto slock = state->guarded.lockExclusive();

        while (index >= slock->regions.size()) {
          auto newRegion = kj::heapArray<byte>(size);
          memset(newRegion.begin(), 0, size);
          slock->regions.add(kj::mv(newRegion));
        }

        return slock->regions[index];
      } else {
        auto slock = state->guarded.lockShared();

        if (index >= slock->regions.size()) {
          return nullptr;
        } else {
          kj::ArrayPtr<const byte> region = slock->regions[index];
          // const_cast OK because the caller will carefully control access to shared memory.
          return kj::arrayPtr(const_cast<byte*>(region.begin()), region.size());
        }
      }
    }

    void clearSharedMemory() override {
      auto slock = state->guarded.lockExclusive();
      slock->regions.clear();
    }

    bool tryLockWalShared(uint start, uint count) override {
      auto slock = state->guarded.lockExclusive();

      for (uint i = start; i < start + count; i++) {
        if (slock->walLocks[i] == (uint)kj::maxValue) {
          // blocked by exclusive lock
          return false;
        }
      }
      for (uint i = start; i < start + count; i++) {
        ++slock->walLocks[i];
      }
      return true;
    }
    bool tryLockWalExclusive(uint start, uint count) override {
      auto slock = state->guarded.lockExclusive();

      for (uint i = start; i < start + count; i++) {
        if (slock->walLocks[i] != 0) {
          // blocked by another lock
          return false;
        }
      }
      for (uint i = start; i < start + count; i++) {
        slock->walLocks[i] = kj::maxValue;
      }
      return true;
    }

    void unlockWalShared(uint start, uint count) override {
      auto slock = state->guarded.lockExclusive();
      for (uint i = start; i < start + count; i++) {
        KJ_ASSERT(slock->walLocks[i] != 0);
        --slock->walLocks[i];
      }
    }
    void unlockWalExclusive(uint start, uint count) override {
      auto slock = state->guarded.lockExclusive();
      for (uint i = start; i < start + count; i++) {
        KJ_REQUIRE(slock->walLocks[i] == (uint)kj::maxValue);
        slock->walLocks[i] = 0;
      }
    }

  private:
    const DefaultLockManager& lockManager;
    kj::Own<LockState> state;
    Level currentLevel = UNLOCKED;
  };
};

SqliteDatabase::Vfs::Vfs(const kj::Directory& directory)
    : directory(directory),
      ownLockManager(kj::heap<DefaultLockManager>()),
      lockManager(*ownLockManager),
      native(*sqlite3_vfs_find(nullptr)) {
#if _WIN32
  vfs = kj::heap(makeKjVfs());
#else
  KJ_IF_MAYBE(fd, directory.getFd()) {
    rootFd = *fd;
    vfs = kj::heap(makeWrappedNativeVfs());
  } else {
    vfs = kj::heap(makeKjVfs());
  }
#endif
  sqlite3_vfs_register(vfs, false);
}

SqliteDatabase::Vfs::Vfs(const kj::Directory& directory, const LockManager& lockManager)
    : directory(directory),
      lockManager(lockManager),
      native(*sqlite3_vfs_find(nullptr)),
      // Always use KJ VFS when using a custom LockManager.
      vfs(kj::heap(makeKjVfs())) {
  sqlite3_vfs_register(vfs, false);
}

SqliteDatabase::Vfs::~Vfs() noexcept(false) {
  sqlite3_vfs_unregister(vfs);
}

kj::String SqliteDatabase::Vfs::makeName() {
  // A pointer to this object should be suitably unique. (Ugghhhh.)
  return kj::str("kj-", this);
}

#if _WIN32
kj::Maybe<kj::Path> SqliteDatabase::Vfs::tryAppend(kj::PathPtr suffix) const {
  auto handle = KJ_UNWRAP_OR_RETURN(directory.getWin32Handle(), nullptr);
  auto root = getPathFromWin32Handle(handle);
  return root.append(suffix);
}
#else
kj::Maybe<kj::Path> SqliteDatabase::Vfs::tryAppend(kj::PathPtr suffix) const {
  // TODO(someday): consider implementing this on other platforms
  return nullptr;
}
#endif

// =======================================================================================

}  // namespace workerd
