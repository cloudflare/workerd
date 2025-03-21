// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite.h"

#include <workerd/util/sentry.h>

#include <kj/debug.h>
#include <kj/refcount.h>
#include <kj/string-tree.h>

#if _WIN32
#include <windows.h>

#include <kj/win32-api-version.h>
#include <kj/windows-sanity.h>
#else
#include <unistd.h>
#endif

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>

#include <kj/map.h>
#include <kj/mutex.h>
#include <kj/vector.h>

#include <atomic>

#if _WIN32
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#endif

namespace workerd {

namespace {

// SQLite has a function like this in its internals, but it's not exposed to library consumers.
//
// These error codes come from https://www.sqlite.org/rescode.html#primary_result_code_list.
kj::String namedErrorCode(int errorCode) {
#define LITERAL(name)                                                                              \
  case name:                                                                                       \
    return kj::str(#name);
  switch (errorCode) {
    LITERAL(SQLITE_OK)
    LITERAL(SQLITE_ERROR)
    LITERAL(SQLITE_INTERNAL)
    LITERAL(SQLITE_PERM)
    LITERAL(SQLITE_ABORT)
    LITERAL(SQLITE_BUSY)
    LITERAL(SQLITE_LOCKED)
    LITERAL(SQLITE_NOMEM)
    LITERAL(SQLITE_READONLY)
    LITERAL(SQLITE_INTERRUPT)
    LITERAL(SQLITE_IOERR)
    LITERAL(SQLITE_CORRUPT)
    LITERAL(SQLITE_NOTFOUND)
    LITERAL(SQLITE_FULL)
    LITERAL(SQLITE_CANTOPEN)
    LITERAL(SQLITE_PROTOCOL)
    LITERAL(SQLITE_EMPTY)
    LITERAL(SQLITE_SCHEMA)
    LITERAL(SQLITE_TOOBIG)
    LITERAL(SQLITE_CONSTRAINT)
    LITERAL(SQLITE_MISMATCH)
    LITERAL(SQLITE_MISUSE)
    LITERAL(SQLITE_NOLFS)
    LITERAL(SQLITE_AUTH)
    LITERAL(SQLITE_FORMAT)
    LITERAL(SQLITE_RANGE)
    LITERAL(SQLITE_NOTADB)
    LITERAL(SQLITE_NOTICE)
    LITERAL(SQLITE_WARNING)
    LITERAL(SQLITE_ROW)
    LITERAL(SQLITE_DONE)
    default:
      return kj::str("SQLITE_UNKNOWN_ERROR_CODE(", errorCode, ")");
  }
#undef LITERAL
}

kj::String dbErrorMessage(int errorCode, sqlite3* db) {
  kj::StringTree msg = kj::strTree(sqlite3_errmsg(db));
  if (int offset = sqlite3_error_offset(db); offset != -1) {
    msg = kj::strTree(kj::mv(msg), " at offset ", offset);
  }
  msg = kj::strTree(kj::mv(msg), ": ", namedErrorCode(errorCode));
  return msg.flatten();
}

// If a VFS call throws an exception, and vfsErrorListener is non-null, the exception will
// be placed there, otherwise it will be logged. This is used to implement pass-through of KJ
// exceptions through SQLite.
static thread_local kj::Maybe<kj::Exception>* vfsErrorListener = nullptr;

// Report that in a sqlite VFS callback, an exception was caught, and SQLITE_IOERROR is being
// returned to SQLite.
//
// The exception must be caught using `catch (kj::Exception& e)`, NOT using `catch (...)` followed
// by kj::getCaughtExceptionAsKj(). This is because the latter truncates the stack trace to show
// only the frames between the throw and the catch. We actually want to retain the full trace
// through SQLite.
void reportVfsErrorCaught(kj::Exception&& e) {
  if (vfsErrorListener != nullptr) {
    // Only capture the first error; assume subsequent errors are side effects.
    if (*vfsErrorListener == kj::none) {
      *vfsErrorListener = kj::mv(e);
    }
  } else {
    LOG_EXCEPTION("sqliteVfsError", e);
  }
}

// Implements SQLITE_CALL_SCOPE.
class SqliteCallScope {
 public:
  SqliteCallScope() {
    KJ_DASSERT(vfsErrorListener == nullptr);
    vfsErrorListener = &error;
  }
  ~SqliteCallScope() {
    vfsErrorListener = nullptr;
  }

  void rethrowVfsError() {
    KJ_IF_SOME(e, error) {
      // Slight hack: The exception already has a stack trace attached which should include the
      // current stack, but `kj::throwFatalException()` would re-append the current stack trace
      // to the exception. We can avoid that by calling
      // kj::getExceptionCallback().onFatalException() directly, which is what
      // `throwFatalException()` does after extending the stack.
      kj::getExceptionCallback().onFatalException(kj::mv(e));
    }
  }

  // Hack to allow block syntax with for(); see SQLITE_CALL_SCOPE.
  bool done = false;

 private:
  kj::Maybe<kj::Exception> error;
};

}  // namespace

// Like KJ_REQUIRE() but give the Regulator a chance to report the error. `errorMessage` is either
// the return value of sqlite3_errmsg() or a string literal containing a similarly
// application-approriate error message. A reference called `regulator` must be in-scope.
// sqliteErrorCode is a kj::Maybe<int> and represents the error code from sqlite.
#define SQLITE_REQUIRE(condition, sqliteErrorCode, errorMessage, ...)                              \
  if (!(condition)) {                                                                              \
    regulator.onError(sqliteErrorCode, errorMessage);                                              \
    KJ_FAIL_REQUIRE("SQLite failed", errorMessage, ##__VA_ARGS__);                                 \
  }

// Make a SQLite call and check the returned error code. Use this version when the call is not
// associated with an open DB connection.
#define SQLITE_CALL_NODB(code, ...)                                                                \
  do {                                                                                             \
    int _ec = code;                                                                                \
    KJ_ASSERT(                                                                                     \
        _ec == SQLITE_OK, kj::str(sqlite3_errstr(_ec), ": ", namedErrorCode(_ec)), ##__VA_ARGS__); \
  } while (false)

// This version requires the scope to contain a variable named `db` which is of type sqlite3*, or
// can convert to it.
#define SQLITE_CALL(code, ...)                                                                     \
  do {                                                                                             \
    SqliteCallScope sqliteCallScope;                                                               \
    int _ec = code;                                                                                \
    /* SQLITE_MISUSE doesn't put error info on the database object, so check it separately */      \
    KJ_ASSERT(_ec != SQLITE_MISUSE, "SQLite misused: " #code, ##__VA_ARGS__);                      \
    if (_ec == SQLITE_IOERR) sqliteCallScope.rethrowVfsError();                                    \
    SQLITE_REQUIRE(_ec == SQLITE_OK, _ec, dbErrorMessage(_ec, db), ##__VA_ARGS__);                 \
  } while (false)

// Version of `SQLITE_CALL` that can be called after inspecting the error code, in case some codes
// aren't really errors.
#define SQLITE_CALL_FAILED(code, error, ...)                                                       \
  do {                                                                                             \
    KJ_ASSERT(error != SQLITE_MISUSE, "SQLite misused: " code, ##__VA_ARGS__);                     \
    if (error == SQLITE_IOERR) sqliteCallScope.rethrowVfsError();                                  \
    SQLITE_REQUIRE(error == SQLITE_OK, error, dbErrorMessage(error, db), ##__VA_ARGS__);           \
  } while (false);

// When using SQLITE_CALL_FAILED(), you must place the actual sqlite call and the
// SQLITE_CALL_FAILED() invocation within a SQLITE_CALL_SCOPE block, in order to set up VFS error
// capture. Example:
//
//   SQLITE_CALL_SCOPE {
//     int errorCode = sqlite3_do_something();
//     if (errorCode != SQLITE_OK) {
//       SQLITE_CALL_FAILED("sqlite3_do_something()", errorCode, "failed to do something");
//     }
//   }
//
// Note that if you use SQLITE_CALL(), this is handled automatically.
#define SQLITE_CALL_SCOPE                                                                          \
  for (SqliteCallScope sqliteCallScope; !sqliteCallScope.done; sqliteCallScope.done = true)

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
      return kj::Path::parseWin32Api(temp.first(len));
    }
    // Try again with new length.
    tryLen = len;
  }
}
#endif

kj::Maybe<kj::StringPtr> toMaybeString(const char* cstr) {
  if (cstr == nullptr) {
    return kj::none;
  } else {
    return kj::StringPtr(cstr);
  }
}

// We allowlist these SQLite functions.
static constexpr kj::StringPtr ALLOWED_SQLITE_FUNCTIONS[] = {
  // https://www.sqlite.org/lang_corefunc.html
  "abs"_kj,
  "changes"_kj,
  "char"_kj,
  "coalesce"_kj,
  "concat"_kj,
  "concat_ws"_kj,
  "format"_kj,
  "glob"_kj,
  "hex"_kj,
  "ifnull"_kj,
  "iif"_kj,
  "instr"_kj,
  "last_insert_rowid"_kj,
  "length"_kj,
  "like"_kj,
  "likelihood"_kj,
  "likely"_kj,
  "load_extension"_kj,
  "lower"_kj,
  "ltrim"_kj,
  "max_scalar"_kj,
  "min_scalar"_kj,
  "nullif"_kj,
  "octet_length"_kj,
  "printf"_kj,
  "quote"_kj,
  "random"_kj,
  "randomblob"_kj,
  "replace"_kj,
  "round"_kj,
  "rtrim"_kj,
  "sign"_kj,
  "soundex"_kj,
  // These functions query SQLite internals and build details in a way we'd prefer not to reveal.
  // "sqlite_compileoption_get"_kj,
  // "sqlite_compileoption_used"_kj,
  // "sqlite_offset"_kj,
  // "sqlite_source_id"_kj,
  // "sqlite_version"_kj,
  "substr"_kj,
  "substring"_kj,
  "total_changes"_kj,
  "trim"_kj,
  "typeof"_kj,
  "unhex"_kj,
  "unicode"_kj,
  "unlikely"_kj,
  "upper"_kj,
  "zeroblob"_kj,

  // https://www.sqlite.org/lang_datefunc.html
  "date"_kj,
  "time"_kj,
  "datetime"_kj,
  "julianday"_kj,
  "unixepoch"_kj,
  "strftime"_kj,
  "timediff"_kj,
  "current_date"_kj,
  "current_time"_kj,
  "current_timestamp"_kj,

  // https://www.sqlite.org/lang_aggfunc.html
  "avg"_kj,
  "count"_kj,
  "group_concat"_kj,
  "max"_kj,
  "min"_kj,
  "string_agg"_kj,
  "sum"_kj,
  "total"_kj,

  // https://www.sqlite.org/windowfunctions.html#biwinfunc
  "row_number"_kj,
  "rank"_kj,
  "dense_rank"_kj,
  "percent_rank"_kj,
  "cume_dist"_kj,
  "ntile"_kj,
  "lag"_kj,
  "lead"_kj,
  "first_value"_kj,
  "last_value"_kj,
  "nth_value"_kj,

  // https://www.sqlite.org/lang_mathfunc.html
  "acos"_kj,
  "acosh"_kj,
  "asin"_kj,
  "asinh"_kj,
  "atan"_kj,
  "atan2"_kj,
  "atanh"_kj,
  "ceil"_kj,
  "cos"_kj,
  "cosh"_kj,
  "degrees"_kj,
  "exp"_kj,
  "floor"_kj,
  "ln"_kj,
  "log"_kj,
  "log2"_kj,
  "mod"_kj,
  "pi"_kj,
  "pow"_kj,
  "radians"_kj,
  "sin"_kj,
  "sinh"_kj,
  "sqrt"_kj,
  "tan"_kj,
  "tanh"_kj,
  "trunc"_kj,

  // https://www.sqlite.org/json1.html
  "json"_kj,
  "json_array"_kj,
  "json_array_length"_kj,
  "json_extract"_kj,
  "->"_kj,
  "->>"_kj,
  "json_insert"_kj,
  "json_object"_kj,
  "json_patch"_kj,
  "json_remove"_kj,
  "json_replace"_kj,
  "json_set"_kj,
  "json_type"_kj,
  "json_valid"_kj,
  "json_quote"_kj,
  "json_group_array"_kj,
  "json_group_object"_kj,
  "json_each"_kj,
  "json_tree"_kj,

  // https://www.sqlite.org/fts5.html
  "match"_kj,
  "highlight"_kj,
  "bm25"_kj,
  "snippet"_kj,

  // https://www.sqlite.org/lang_altertable.html
  // Functions declared in https://sqlite.org/src/file?name=src/alter.c&ci=trunk
  "sqlite_rename_column"_kj,
  "sqlite_rename_table"_kj,
  "sqlite_rename_test"_kj,
  "sqlite_drop_column"_kj,
  "sqlite_rename_quotefix"_kj,
};

enum class PragmaSignature {
  NO_ARG,
  BOOLEAN,
  OBJECT_NAME,
  OPTIONAL_OBJECT_NAME,
  NULL_OR_NUMBER,
  NULL_NUMBER_OR_OBJECT_NAME
};
struct PragmaInfo {
  kj::StringPtr name;
  PragmaSignature signature;
};

// We allowlist these SQLite pragmas (for read only, never with arguments).
// https://www.sqlite.org/pragma.html
static constexpr PragmaInfo ALLOWED_PRAGMAS[] = {{"data_version"_kj, PragmaSignature::NO_ARG},

  // We allowlist some SQLite pragmas for changing internal state

  // Toggle constraints on/off
  {"case_sensitive_like"_kj, PragmaSignature::BOOLEAN},
  {"foreign_keys"_kj, PragmaSignature::BOOLEAN},
  {"defer_foreign_keys"_kj, PragmaSignature::BOOLEAN},
  {"ignore_check_constraints"_kj, PragmaSignature::BOOLEAN},
  {"legacy_alter_table"_kj, PragmaSignature::BOOLEAN},
  {"recursive_triggers"_kj, PragmaSignature::BOOLEAN},
  {"reverse_unordered_selects"_kj, PragmaSignature::BOOLEAN},

  // Takes an argument of table name or index name, returns info about it.
  {"foreign_key_check"_kj, PragmaSignature::OPTIONAL_OBJECT_NAME},
  {"foreign_key_list"_kj, PragmaSignature::OBJECT_NAME},
  {"index_info"_kj, PragmaSignature::OBJECT_NAME}, {"index_list"_kj, PragmaSignature::OBJECT_NAME},
  {"index_xinfo"_kj, PragmaSignature::OBJECT_NAME},

  // Takes an argument of table name/index name OR a max number of results, or nothing
  {"quick_check"_kj, PragmaSignature::NULL_NUMBER_OR_OBJECT_NAME},

  // Takes a number representing a bit mask or nothing to use the default mask.
  {"optimize"_kj, PragmaSignature::NULL_OR_NUMBER}};

}  // namespace

// =======================================================================================

SqliteObserver SqliteObserver::DEFAULT = SqliteObserver{};

constexpr SqliteDatabase::Regulator SqliteDatabase::TRUSTED;

SqliteDatabase::SqliteDatabase(const Vfs& vfs,
    kj::Path path,
    kj::Maybe<kj::WriteMode> maybeMode,
    SqliteObserver& sqliteObserver)
    : vfs(vfs),
      path(kj::mv(path)),
      readOnly(maybeMode == kj::none),
      sqliteObserver(sqliteObserver) {
  init(maybeMode);
}

void SqliteDatabase::init(kj::Maybe<kj::WriteMode> maybeMode) {
  KJ_ASSERT(maybeDb == kj::none);
  sqlite3* db = nullptr;

  KJ_IF_SOME(mode, maybeMode) {
    int flags = SQLITE_OPEN_READWRITE;
    if (kj::has(mode, kj::WriteMode::CREATE)) {
      flags |= SQLITE_OPEN_CREATE;

      if (kj::has(mode, kj::WriteMode::CREATE_PARENT) && path.size() > 1) {
        // SQLite isn't going to try to create the parent directory so let's try to create it now.
        vfs.directory.openSubdir(path.parent(),
            kj::WriteMode::CREATE | kj::WriteMode::MODIFY | kj::WriteMode::CREATE_PARENT);
      }
    }
    KJ_REQUIRE(
        kj::has(mode, kj::WriteMode::MODIFY), "SQLite doesn't support create-exclusive mode");

    KJ_IF_SOME(rootedPath, vfs.tryAppend(path)) {
      // If we can get the path rooted in the VFS's directory, use the system's default VFS instead
      // TODO(bug): This doesn't honor vfs.options. (This branch is only used on Windows.)
      SQLITE_CALL_NODB(sqlite3_open_v2(rootedPath.toString().cStr(), &db, flags, nullptr));
    } else {
      SQLITE_CALL_NODB(sqlite3_open_v2(path.toString().cStr(), &db, flags, vfs.getName().cStr()));
    }
  } else {
    KJ_IF_SOME(rootedPath, vfs.tryAppend(path)) {
      // If we can get the path rooted in the VFS's directory, use the system's default VFS instead
      // TODO(bug): This doesn't honor vfs.options. (This branch is only used on Windows.)
      SQLITE_CALL_NODB(
          sqlite3_open_v2(rootedPath.toString().cStr(), &db, SQLITE_OPEN_READONLY, nullptr));
    } else {
      SQLITE_CALL_NODB(
          sqlite3_open_v2(path.toString().cStr(), &db, SQLITE_OPEN_READONLY, vfs.getName().cStr()));
    }
  }

  KJ_ON_SCOPE_FAILURE(sqlite3_close_v2(db));

  setupSecurity(db);

  maybeDb = *db;
}

SqliteDatabase::~SqliteDatabase() noexcept(false) {
  sqlite3* db = &KJ_UNWRAP_OR(maybeDb, return);

  auto err = sqlite3_close(db);
  if (err == SQLITE_BUSY) {
    KJ_LOG(ERROR, "sqlite database destroyed while dependent objects still exist");
    // SQLite actually provides a lazy-close API which we might as well use here instead of leaking
    // memory.
    err = sqlite3_close_v2(db);
  }

  KJ_REQUIRE(err == SQLITE_OK, sqlite3_errstr(err)) {
    break;
  }
}

SqliteDatabase::operator sqlite3*() {
  return &KJ_ASSERT_NONNULL(maybeDb, "previous reset() failed");
}

void SqliteDatabase::notifyWrite() {
  KJ_IF_SOME(cb, onWriteCallback) {
    cb();
  }
}

kj::StringPtr SqliteDatabase::getCurrentQueryForDebug() {
  KJ_IF_SOME(s, currentStatement) {
    return sqlite3_normalized_sql(&s);
  } else {
    return "(no statement is running)";
  }
}

void SqliteDatabase::applyChange(const StateChange& change) {
  KJ_SWITCH_ONEOF(change) {
    KJ_CASE_ONEOF(none, NoChange) {
      // Nothing.
    }

    KJ_CASE_ONEOF(begin, BeginTxn) {
      KJ_IF_SOME(name, begin.savepointName) {
        savepoints.add(
            Savepoint{.name = kj::str(name), .rollbackCallbackIndex = rollbackCallbacks.size()});
      } else {
        KJ_ASSERT(savepoints.empty(),
            "BEGIN TRANSACTION should have failed when savepoints are present?");
        KJ_ASSERT(
            !inTransaction, "BEGIN TRANSACTION should have failed when already in a transaction?");
        KJ_ASSERT(rollbackCallbacks.empty(),
            "we shouldn't have been keeping rollback callbacks with no transaction open!");
        inTransaction = true;
      }
    }

    KJ_CASE_ONEOF(commit, CommitTxn) {
      KJ_IF_SOME(name, commit.savepointName) {
        // According to https://www.sqlite.org/lang_savepoint.html, releasing a savepoint also
        // releases all later savepoints. In theory it seems like savepoints shouldn't need to
        // be LIFO like this, but the docs say they are!
        for (;;) {
          KJ_ASSERT(!savepoints.empty(), "released a savepoint that didn't exist?");
          auto sp = kj::mv(savepoints.back());
          savepoints.removeLast();
          if (sp.name == name) break;
        }
      } else {
        KJ_ASSERT(inTransaction, "COMMIT TRANSACTION without BEGIN TRANSACTION?");

        // Since BEGIN TRANSACTION cannot be nested within a savepoint, this must have released
        // all savepoints implicitly.
        savepoints.clear();
        inTransaction = false;
      }

      if (savepoints.empty() && !inTransaction) {
        // Transaction stack is empty, so the transaction is committed. We can release the rollback
        // callbacks.
        rollbackCallbacks.clear();
      }
    }

    KJ_CASE_ONEOF(rollback, RollbackTxn) {
      KJ_IF_SOME(name, rollback.savepointName) {
        for (;;) {
          KJ_ASSERT(!savepoints.empty(), "released a savepoint that didn't exist?");
          if (savepoints.back().name == name) {
            // Found the savepoint.
            // Call all rollback callbacks later than the savepoint.
            size_t index = savepoints.back().rollbackCallbackIndex;
            KJ_ASSERT(rollbackCallbacks.size() >= index);
            while (rollbackCallbacks.size() > index) {
              rollbackCallbacks.back()();
              rollbackCallbacks.removeLast();
            }

            // NOTE: Rolling back to a savepoint does not actually release the savepoint. Hence
            //   we save this savepoint as the last item in `savepoints`. It must be released
            //   separately.
            break;
          }

          savepoints.removeLast();
        }
      } else {
        KJ_ASSERT(inTransaction, "ROLLBACK TRANSACTION without BEGIN TRANSACTION?");

        savepoints.clear();
        inTransaction = false;

        while (!rollbackCallbacks.empty()) {
          rollbackCallbacks.back()();
          rollbackCallbacks.removeLast();
        }
      }
    }
  }
}

// Set up the regulator that will be used for authorizer callbacks while preparing this
// statement.
SqliteDatabase::StatementAndEffect SqliteDatabase::prepareSql(const Regulator& regulator,
    kj::StringPtr sqlCode,
    uint prepFlags,
    Multi multi,
    kj::Maybe<kj::Vector<Statement>&> prelude) {
  sqlite3* db = &KJ_ASSERT_NONNULL(maybeDb, "previous reset() failed");

  ParseContext parseContext;
  KJ_ASSERT(currentParseContext == kj::none, "recursive prepareSql()?");
  KJ_DEFER(currentParseContext = kj::none);
  currentParseContext = parseContext;

  KJ_ASSERT(currentRegulator == kj::none,
      "can't prepare statements inside executeWithRegulator() callback");
  KJ_DEFER(currentRegulator = kj::none);
  currentRegulator = regulator;

  // If we fail, we need to discard any statements we added to the prelude, because the next time
  // the statement runs they'll be parsed again and added again.
  uint preludeInitialSize = 0;
  KJ_IF_SOME(p, prelude) {
    preludeInitialSize = p.size();
  }
  KJ_ON_SCOPE_FAILURE({
    KJ_IF_SOME(p, prelude) {
      while (p.size() > preludeInitialSize) {
        p.removeLast();
      }
    } else {
      // (else block needed to squelch spurious clang warning)
    }
  });

  for (;;) {
    sqlite3_stmt* result;
    const char* tail;

    SQLITE_CALL_SCOPE {
      auto prepareResult =
          sqlite3_prepare_v3(db, sqlCode.begin(), sqlCode.size(), prepFlags, &result, &tail);

      // If we had an auth error specifically, check if we recorded a better error message during
      // the authorizer callback.
      if (prepareResult == SQLITE_AUTH) {
        KJ_IF_SOME(error, parseContext.authError) {
          // Throw the tailored auth error.
          kj::throwFatalException(kj::mv(error));
        }
        // we don't have a better error, so fall back to SQLITE_CALL_FAILED below
      }

      if (prepareResult != SQLITE_OK) {
        SQLITE_CALL_FAILED("sqlite3_prepare_v3", prepareResult);
      }
    }

    SQLITE_REQUIRE(result != nullptr, kj::none, "SQL code did not contain a statement.", sqlCode);
    auto ownResult = ownSqlite(result);

    while (*tail == ' ' || *tail == '\t' || *tail == '\n' || *tail == '\r' || *tail == '\v' ||
        *tail == '\f')
      ++tail;

    switch (multi) {
      case SINGLE:
        SQLITE_REQUIRE(tail == sqlCode.end(), kj::none,
            "A prepared SQL statement must contain only one statement.", tail);
        break;

      case MULTI:
        if (tail != sqlCode.end()) {
          // There are more statements after this one, so execute this statement now.

          SQLITE_REQUIRE(sqlite3_bind_parameter_count(result) == 0, kj::none,
              "When executing multiple SQL statements in a single call, only the last statement "
              "can have parameters.");

          // Be sure to call the onWrite callback if necessary for this statement.
          KJ_IF_SOME(cb, onWriteCallback) {
            if (!sqlite3_stmt_readonly(result)) {
              // The callback is allowed to invoke queries of its own, so we have to un-set the
              // regulator and parse context while we call it.
              currentRegulator = kj::none;
              KJ_DEFER(currentRegulator = regulator);
              currentParseContext = kj::none;
              KJ_DEFER(currentParseContext = parseContext);
              cb();
            }
          }

          // This isn't the last statement in the code. Execute it immediately.
          SQLITE_CALL_SCOPE {
            int err = sqlite3_step(result);
            if (err == SQLITE_DONE) {
              // good
            } else if (err == SQLITE_ROW) {
              // Intermediate statement returned results. We will discard.
            } else {
              SQLITE_CALL_FAILED("sqlite3_step()", err);
            }
          }

          // Apply any state changes from executing the statement.
          applyChange(parseContext.stateChange);

          KJ_IF_SOME(p, prelude) {
            p.add(Statement(*this, regulator,
                StatementAndEffect{.statement = kj::mv(ownResult),
                  .stateChange = kj::mv(parseContext.stateChange)}));
          }

          // Reset parse context for next statement.
          parseContext = {};

          // Reduce `sqlCode` to include only what we haven't already executed.
          sqlCode = kj::StringPtr(tail, sqlCode.end());

          continue;
        }
        break;
    }

    return {.statement = kj::mv(ownResult), .stateChange = kj::mv(parseContext.stateChange)};
  }
}

SqliteDatabase::IngestResult SqliteDatabase::ingestSql(
    const Regulator& regulator, kj::StringPtr sqlCode) {
  uint64_t rowsRead = 0;
  uint64_t rowsWritten = 0;
  uint64_t statementCount = 0;

  // While there's still some input SQL to process
  while (sqlCode.begin() != sqlCode.end()) {
    // And there are still valid statements:
    auto statementLength = sqlite3_complete_length(sqlCode.begin(), 1);
    if (!statementLength) break;

    // Slice off the next valid statement SQL
    auto nextStatement = kj::str(sqlCode.first(statementLength));
    // Create a Query object, which will prepare & execute it
    auto q = Query(*this, regulator, nextStatement);

    rowsRead += q.getRowsRead();
    rowsWritten += q.getRowsWritten();
    statementCount++;
    sqlCode = sqlCode.slice(statementLength);
  }

  // Return the leftover buffer
  return {.remainder = sqlCode,
    .rowsRead = rowsRead,
    .rowsWritten = rowsWritten,
    .statementCount = statementCount};
}

void SqliteDatabase::executeWithRegulator(
    const Regulator& regulator, kj::FunctionParam<void()> func) {
  // currentRegulator would only be set if we're running this method while running something else
  // with a regulator.  I'm not sure what the ramifications are, so for now, we'll just assume that
  // we can only call executeWithRegulator when no regulator is currently set.
  KJ_REQUIRE(currentRegulator == kj::none);

  currentRegulator = regulator;
  KJ_DEFER(currentRegulator = kj::none);
  func();
}

void SqliteDatabase::reset() {
  KJ_REQUIRE(!readOnly, "can't reset() read-only database");

  // If transactions are open during reset(), whatever had the transaction open is going to get
  // confused at best, or lose data at worst. Let's just not allow this.
  KJ_REQUIRE(!inTransaction && savepoints.empty(), "can't reset() a database during a transaction");

  // Temporarily disable the on-write callback while resetting.
  auto writeCb = kj::mv(onWriteCallback);
  KJ_DEFER(onWriteCallback = kj::mv(writeCb));

  KJ_IF_SOME(db, maybeDb) {
    for (auto& listener: resetListeners) {
      listener.beforeSqliteReset();
    }

    auto err = sqlite3_close(&db);
    KJ_REQUIRE(err == SQLITE_OK, "can't reset() database because dependent objects still exist",
        sqlite3_errstr(err));

    maybeDb = kj::none;
    vfs.directory.remove(path);
  }

  KJ_ON_SCOPE_FAILURE(maybeDb = kj::none);
  init(kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

  KJ_IF_SOME(resetCb, afterResetCallback) {
    resetCb(*this);
  }
}

bool SqliteDatabase::isAuthorized(int actionCode,
    kj::Maybe<kj::StringPtr> param1,
    kj::Maybe<kj::StringPtr> param2,
    kj::Maybe<kj::StringPtr> dbName,
    kj::Maybe<kj::StringPtr> triggerName) {
  const Regulator& regulator = KJ_UNWRAP_OR(currentRegulator, {
    // We're not currently preparing a statement, so we didn't expect the authorizer callback to
    // run. We blanket-deny in this case as a precaution.
    KJ_LOG(ERROR, "SQLite authorizer callback invoked at unexpected time", kj::getStackTrace());
    return false;
  });

  KJ_IF_SOME(t, triggerName) {
    if (!regulator.isAllowedTrigger(t)) {
      // Log an error because it seems really suspicious if a trigger runs when it's not allowed.
      // I want to understand if this can even happen.
      KJ_LOG(ERROR, "disallowed trigger somehow ran in trusted scope?", t, kj::getStackTrace());

      // TODO(security): Is it better to return SQLITE_IGNORE to ignore the trigger? I don't fully
      //   understand the implications of SQLITE_IGNORE. The documentation mentions that in the
      //   case of SQLITE_DELETE, it doesn't actually ignore the delete, which is weird. Hopefully
      //   it's impossible for people to register a trigger on protected tables in the first place,
      //   so triggers will never run.
      return false;
    }
  }

  // For some reason, for these two operations, SQLite sends the DB Name through as param1, with
  // the table name (for ALTER_TABLE) in param2 instead of param1 like all other table operations.
  // For simplicity, and because the following comment precedes sqlite3_set_authorizer in sqlite.h:
  //
  //     > The 5th parameter to the authorizer callback is the name of the database
  //     > ("main", "temp", etc.) if applicable.
  //
  // we are treating this as an SQLite bug and swapping the values around.
  if (actionCode == SQLITE_ALTER_TABLE || actionCode == SQLITE_DETACH) {
    auto swap = param1;  // contains dbName
    param1 = param2;     // contains table name (for SQLITE_ALTER_TABLE, null otherwise)
    param2 = dbName;     // should always be null
    dbName = swap;
  }

  KJ_IF_SOME(d, dbName) {
    if (d == "temp"_kj) {
      return isAuthorizedTemp(actionCode, param1, param2, regulator);
    } else if (d != "main"_kj) {
      // We don't allow opening multiple databases (except for 'main' and the 'temp'
      // temporary database), as our storage engine is not designed to track multiple
      // files on-disk.
      return false;
    }
  }

  if (&regulator == &TRUSTED && actionCode != SQLITE_TRANSACTION &&
      actionCode != SQLITE_SAVEPOINT) {
    // Everything is allowed for trusted queries. (But transactions and savepoints need special
    // handling below.)
    return true;
  }

  switch (actionCode) {
      // ---------------------------------------------------------------
      // Stuff that is (sometimes) allowed

    case SQLITE_SELECT: /* NULL            NULL            */
      // Yes, SELECT statements are allowed. (Note that if the SELECT names any tables, a separate
      // SQLITE_READ will be authorized for each one.)
      KJ_ASSERT(param1 == kj::none);
      KJ_ASSERT(param2 == kj::none);
      return true;

    case SQLITE_CREATE_TABLE: /* Table Name      NULL            */
    case SQLITE_DELETE:       /* Table Name      NULL            */
    case SQLITE_DROP_TABLE:   /* Table Name      NULL            */
    case SQLITE_INSERT:       /* Table Name      NULL            */
    case SQLITE_CREATE_VIEW:  /* View Name       NULL            */
    case SQLITE_DROP_VIEW:    /* View Name       NULL            */
    case SQLITE_REINDEX:      /* Index Name      NULL            */
      KJ_ASSERT(param2 == kj::none);
      return regulator.isAllowedName(KJ_ASSERT_NONNULL(param1));

    case SQLITE_ANALYZE: /* Table Name      NULL            */
      KJ_ASSERT(param2 == kj::none);
      // We allow all names (including names where isAllowedName() would return false) because
      // `PRAGMA optimize` issues an ANALYZE statement with no arguments and a SQLite ANALYZE
      // statement with no parameters will analyze all tables, including otherwise restricted
      // tables.
      //
      // The ANALYZE statement records information about the distribution of rows in each index in
      // the database in a special sqlite_stat1 table.  While the sqlite_stat1 table leaks metadata
      // about restricted tables (like the names of indices and the sizes of those tables), the
      // sqlite_stat1 does not contain data from the restricted tables.  As such, it's OK to allow
      // users to ANALYZE restricted tables.
      //
      // Note that users can *modify* the sqlite_stat1 table, which means that they can make the
      // query planner work in suboptimal ways by writing bogus data to the table.
      //
      // See https://www.sqlite.org/fileformat2.html#stat1tab for more details.
      return true;

    case SQLITE_ALTER_TABLE: /* Table Name      NULL (modified) */
      return regulator.isAllowedName(KJ_ASSERT_NONNULL(param1));

    case SQLITE_READ:   /* Table Name      Column Name     */
    case SQLITE_UPDATE: /* Table Name      Column Name     */
      return regulator.isAllowedName(KJ_ASSERT_NONNULL(param1));

    case SQLITE_CREATE_INDEX:   /* Index Name      Table Name      */
    case SQLITE_DROP_INDEX:     /* Index Name      Table Name      */
    case SQLITE_CREATE_TRIGGER: /* Trigger Name    Table Name      */
    case SQLITE_DROP_TRIGGER:   /* Trigger Name    Table Name      */
      return regulator.isAllowedName(KJ_ASSERT_NONNULL(param1)) &&
          regulator.isAllowedName(KJ_ASSERT_NONNULL(param2));

    case SQLITE_TRANSACTION: /* Operation       NULL            */
    {
      if (!regulator.allowTransactions()) {
        return false;
      }

      kj::StringPtr op = KJ_ASSERT_NONNULL(param1);
      StateChange change;
      if (op == "BEGIN") {
        change = BeginTxn{kj::none};
      } else if (op == "COMMIT") {
        change = CommitTxn{kj::none};
      } else if (op == "ROLLBACK") {
        change = RollbackTxn{kj::none};
      } else {
        KJ_FAIL_ASSERT("unknown SQLITE_TRANSACTION op", op);
      }
      KJ_IF_SOME(ctx, currentParseContext) {
        ctx.stateChange = kj::mv(change);
      }

      KJ_ASSERT(param2 == kj::none);
      return true;
    }

    case SQLITE_SAVEPOINT: /* Operation       Savepoint Name  */
    {
      kj::String name = kj::str(KJ_ASSERT_NONNULL(param2));
      if (!regulator.allowTransactions() || !regulator.isAllowedName(name)) {
        return false;
      }

      kj::StringPtr op = KJ_ASSERT_NONNULL(param1);
      StateChange change;
      if (op == "BEGIN") {
        change = BeginTxn{kj::mv(name)};
      } else if (op == "RELEASE") {
        change = CommitTxn{kj::mv(name)};
      } else if (op == "ROLLBACK") {
        change = RollbackTxn{kj::mv(name)};
      } else {
        KJ_FAIL_ASSERT("unknown SQLITE_TRANSACTION op", op);
      }
      KJ_IF_SOME(ctx, currentParseContext) {
        ctx.stateChange = kj::mv(change);
      }

      return true;
    }

    case SQLITE_PRAGMA: /* Pragma Name     1st arg or NULL */
      // We currently only permit a few pragmas.
      {
        kj::StringPtr pragma = KJ_ASSERT_NONNULL(param1);

        if (pragma == "table_list") {
          // Annoyingly, this will list internal tables. However, the existence of these tables
          // isn't really a secret, we just don't want people to access them.
          return true;
          // TODO function_list & pragma_list should be authorized but return
          // ALLOWED_SQLITE_FUNCTIONS & ALLOWED_[READ|WRITE]_PRAGMAS
          // respectively
        } else if (pragma == "table_info" || pragma == "table_xinfo") {
          // Allow if the specific named table is not protected.
          KJ_IF_SOME(name, param2) {
            return regulator.isAllowedName(name);
          } else {
            return false;  // shouldn't happen?
          }
        }

        static const kj::HashMap<kj::StringPtr, PragmaSignature> allowedPragmas = []() {
          kj::HashMap<kj::StringPtr, PragmaSignature> result;
          for (auto& [name, signature]: ALLOWED_PRAGMAS) {
            result.insert(name, signature);
          }
          return result;
        }();

        PragmaSignature sig = KJ_UNWRAP_OR(allowedPragmas.find(pragma), return false);
        switch (sig) {
          case PragmaSignature::NO_ARG:
            return param2 == kj::none;
          case PragmaSignature::BOOLEAN: {
            // We allow omitting the argument in order to read back the current value.
            auto val = KJ_UNWRAP_OR(param2, return true).asArray();

            // SQLite offers many different ways to express booleans...

            // They can be quoted. Remove quotes if present.
            if (val.size() >= 2 && (val.front() == '\'' || val.front() == '\"') &&
                val.back() == val.front()) {
              val = val.slice(1, val.size() - 1);
            }

            // Compare against every possible representation. Case-insensitive!
            return strncasecmp(val.begin(), "true", 4) == 0 ||
                strncasecmp(val.begin(), "false", 5) == 0 ||
                strncasecmp(val.begin(), "yes", 3) == 0 || strncasecmp(val.begin(), "no", 2) == 0 ||
                strncasecmp(val.begin(), "on", 2) == 0 || strncasecmp(val.begin(), "off", 3) == 0 ||
                strncasecmp(val.begin(), "1", 1) == 0 || strncasecmp(val.begin(), "0", 1) == 0;
          }
          case PragmaSignature::OBJECT_NAME: {
            // Argument is required.
            auto val = KJ_UNWRAP_OR(param2, return false);
            return regulator.isAllowedName(val);
          }
          case PragmaSignature::OPTIONAL_OBJECT_NAME: {
            auto val = KJ_UNWRAP_OR(param2, return true);
            return regulator.isAllowedName(val);
          }
          case PragmaSignature::NULL_OR_NUMBER: {
            // Argument is not required
            auto val = KJ_UNWRAP_OR(param2, return true);
            // val is allowed if it parses to an integer
            return val.tryParseAs<int32_t>() != kj::none;
          }
          case PragmaSignature::NULL_NUMBER_OR_OBJECT_NAME: {
            // Argument is not required
            auto val = KJ_UNWRAP_OR(param2, return true);
            // val is allowed if it parses to an integer
            if (val.tryParseAs<uint>() != kj::none) return true;
            // Otherwise, val must be the name of an object the user has access to
            return regulator.isAllowedName(val);
          }
        }
        KJ_UNREACHABLE;
      }

      return false;

    case SQLITE_FUNCTION: /* NULL            Function Name   */
    {
      static const kj::HashSet<kj::StringPtr> allowSet = []() {
        kj::HashSet<kj::StringPtr> result;
        for (const kj::StringPtr& func: ALLOWED_SQLITE_FUNCTIONS) {
          result.insert(func);
        }
        return result;
      }();
      return allowSet.contains(KJ_ASSERT_NONNULL(param2));
    }

      // ---------------------------------------------------------------
      // Stuff that is never allowed

    case SQLITE_CREATE_VTABLE: /* Table Name      Module Name     */
    case SQLITE_DROP_VTABLE:   /* Table Name      Module Name     */
      // Virtual tables are tables backed by some native-code callbacks.
      // We don't support these except for FTS5 (Full Text Search) https://www.sqlite.org/fts5.html
      // (Which also includes fts5vocab: "[fts5vocab] is available whenever FTS5 is")
      {
        KJ_IF_SOME(moduleName, param2) {
          if (strcasecmp(moduleName.begin(), "fts5") == 0 ||
              strcasecmp(moduleName.begin(), "fts5vocab") == 0) {
            return true;
          }
        }
        return false;
      }

    case SQLITE_ATTACH: /* Filename        NULL            */
    case SQLITE_DETACH: /* Table Name      NULL (modified) */
      // We do not support attached databases. It seems unlikely that we ever will.
      return false;

    case SQLITE_CREATE_TEMP_TABLE:   /* Table Name      NULL            */
    case SQLITE_DROP_TEMP_TABLE:     /* Table Name      NULL            */
    case SQLITE_CREATE_TEMP_INDEX:   /* Index Name      Table Name      */
    case SQLITE_DROP_TEMP_INDEX:     /* Index Name      Table Name      */
    case SQLITE_CREATE_TEMP_TRIGGER: /* Trigger Name    Table Name      */
    case SQLITE_DROP_TEMP_TRIGGER:   /* Trigger Name    Table Name      */
    case SQLITE_CREATE_TEMP_VIEW:    /* View Name       NULL            */
    case SQLITE_DROP_TEMP_VIEW:      /* View Name       NULL            */
      // TODO(someday): Allow temporary tables. Creating a temporary table actually causes
      //   SQLite to open a separate temporary file to place the data in. Currently, our storage
      //   engine has no support for this.
      return false;

    case SQLITE_RECURSIVE: /* NULL            NULL            */
      // Recursive select, this is fine.
      return true;

    case SQLITE_COPY: /* No longer used */
      // These are operations we simply don't support today.
      return false;

    default:
      KJ_LOG(WARNING, "unknown SQLite action", actionCode);
      return false;
  }
}

// Temp databases have very restricted operations
bool SqliteDatabase::isAuthorizedTemp(int actionCode,
    const kj::Maybe<kj::StringPtr>& param1,
    const kj::Maybe<kj::StringPtr>& param2,
    const Regulator& regulator) {

  switch (actionCode) {
    case SQLITE_READ:   /* Table Name      Column Name     */
    case SQLITE_UPDATE: /* Table Name      Column Name     */
      return regulator.isAllowedName(KJ_ASSERT_NONNULL(param1));
    default:
      return false;
  }
}

// Set up security restrictions.
// See: https://www.sqlite.org/security.html
void SqliteDatabase::setupSecurity(sqlite3* db) {
  // 1. Set defensive mode.
  SQLITE_CALL_NODB(sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr));

  // 2. Reduce limits
  // We use the suggested limits from the web site. Note that sqlite3_limit() does NOT return an
  // error code; it returns the old limit.

  // This limit is set higher than what is suggested on sqlite.org/security.html
  // because we want to allow storing values of 1MiB, and we added some extra
  // padding on top of that
  sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 2200000);
  sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, 100000);
  sqlite3_limit(db, SQLITE_LIMIT_COLUMN, 100);
  sqlite3_limit(db, SQLITE_LIMIT_EXPR_DEPTH, 100);
  // Enforces limits on UNION/UNION ALL/INTERSECT/etc
  // https://www.sqlite.org/limits.html#max_compound_select
  sqlite3_limit(db, SQLITE_LIMIT_COMPOUND_SELECT, 5);
  sqlite3_limit(db, SQLITE_LIMIT_VDBE_OP, 25000);
  // For SQLITE_LIMIT_FUNCTION_ARG we use the default instead of the "security" recommendation
  // because there are too many valid use cases for large argument lists, especially json_object.
  sqlite3_limit(db, SQLITE_LIMIT_FUNCTION_ARG, 127);
  sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, 0);
  sqlite3_limit(db, SQLITE_LIMIT_LIKE_PATTERN_LENGTH, 50);
  sqlite3_limit(db, SQLITE_LIMIT_VARIABLE_NUMBER, 100);
  sqlite3_limit(db, SQLITE_LIMIT_TRIGGER_DEPTH, 10);
  sqlite3_limit(db, SQLITE_LIMIT_WORKER_THREADS, 0);

  // 3. Setup authorizer.
  SQLITE_CALL_NODB(sqlite3_set_authorizer(db,
      [](void* userdata, int actionCode, const char* param1, const char* param2, const char* dbName,
          const char* triggerName) {
    try {
      return reinterpret_cast<SqliteDatabase*>(userdata)->isAuthorized(actionCode,
                 toMaybeString(param1), toMaybeString(param2), toMaybeString(dbName),
                 toMaybeString(triggerName))
          ? SQLITE_OK
          : SQLITE_DENY;
    } catch (kj::Exception& e) {
      // We'll crash if we throw to SQLite. Instead, shove the error into the parse context and
      // report authorization denied. We'll pull it back out later.
      KJ_IF_SOME(context, reinterpret_cast<SqliteDatabase*>(userdata)->currentParseContext) {
        context.authError = kj::mv(e);
      } else {
        KJ_LOG(ERROR, e);
      }
      return SQLITE_DENY;
    }
  },
      this));

  // 4. Set a progress handler or use interrupt() to limit CPU time.
  // This happens inside LimitEnforcer.

  // 5. Limit heap size.
  // Annoyingly, this sets a process-wide limit. We'll set 128MB "soft" limit (to try to control
  // how much page caching SQLite does) and 512MB "hard" limit (to block DoS attacks from taking
  // down the whole system).
  // TODO(perf): Revisit as popularity grows. Maybe make configurable? Maybe patch SQLite to allow
  //   these to be controlled per-database? Is page caching even all that important when the kernel
  //   does its own page caching?
  static bool doOnce KJ_UNUSED = []() {
    sqlite3_soft_heap_limit64(128u << 20);
    sqlite3_hard_heap_limit64(512u << 20);
    return false;
  }();

  // 6. Set SQLITE_MAX_ALLOCATION_SIZE compile flag.
  // (handled in BUILD.sqlite3)

  // 7. Consider giving SQLite a fixed heap space.
  // This is suggested mainly for embedded systems. It involves giving SQLite a fixed preallocated
  // heap space which the library restricts itself to instead of using malloc. We probably don't
  // want this.

  // 8. Set the SQLITE_PRINTF_PRECISION_LIMIT compile flag.
  // (handled in BUILD.sqlite3)
}

SqliteDatabase::Statement SqliteDatabase::prepare(
    const Regulator& regulator, kj::StringPtr sqlCode) {
  return Statement(
      *this, regulator, prepareSql(regulator, sqlCode, SQLITE_PREPARE_PERSISTENT, SINGLE));
}

SqliteDatabase::StatementAndEffect& SqliteDatabase::Statement::prepareForExecution() {
  for (auto& stmt: prelude) {
    stmt.run();
  }

  KJ_IF_SOME(sqlCode, stmt.tryGet<kj::String>()) {
    // Database was reset. Recompile the statement against the new database. (This could throw,
    // of course, if the statement depends on tables that haven't been recreated yet.)
    //
    // We use the MULTI flag here in case this Statement was created by prepareMulti(). If multiple
    // statements are parsed, they'll be added to our `prelude`, and also executed immediately.
    stmt = db.prepareSql(regulator, sqlCode, SQLITE_PREPARE_PERSISTENT, MULTI, prelude);
  }

  return KJ_ASSERT_NONNULL(stmt.tryGet<StatementAndEffect>());
}

void SqliteDatabase::Statement::beforeSqliteReset() {
  KJ_IF_SOME(prepared, stmt.tryGet<StatementAndEffect>()) {
    // Pull the original SQL code out of the statement and store it.
    stmt = kj::str(sqlite3_sql(prepared.statement));
  }
}

SqliteDatabase::Query::Query(SqliteDatabase& db,
    const Regulator& regulator,
    Statement& statement,
    kj::ArrayPtr<const ValuePtr> bindings)
    : ResetListener(db),
      regulator(regulator),
      maybeStatement(statement.prepareForExecution()) {
  // If we throw from the constructor, the destructor won't run. Need to call destroy() explicitly.
  KJ_ON_SCOPE_FAILURE(destroy());
  init(bindings);
}

SqliteDatabase::Query::Query(SqliteDatabase& db,
    const Regulator& regulator,
    kj::StringPtr sqlCode,
    kj::ArrayPtr<const ValuePtr> bindings)
    : ResetListener(db),
      regulator(regulator),
      ownStatement(db.prepareSql(regulator, sqlCode, 0, MULTI)),
      maybeStatement(ownStatement) {
  // If we throw from the constructor, the destructor won't run. Need to call destroy() explicitly.
  KJ_ON_SCOPE_FAILURE(destroy());
  init(bindings);
}

SqliteDatabase::Query::~Query() noexcept(false) {
  destroy();
}

void SqliteDatabase::Query::destroy() {
  if (regulator.shouldAddQueryStats()) {
    //Update the db stats that we have collected for the query
    db.sqliteObserver.addQueryStats(rowsRead, rowsWritten);
  }

  // We only need to reset the statement if we don't own it. If we own it, it's about to be
  // destroyed anyway.
  if (ownStatement.statement.get() == nullptr) {
    KJ_IF_SOME(statement, maybeStatement) {
      // The error code returned by sqlite3_reset() actually represents the last error encountered
      // when stepping the statement. This doesn't mean that the reset failed.
      sqlite3_reset(statement.statement);

      // sqlite3_clear_bindings() returns int, but there is no documentation on how the return code
      // should be interpreted, so we ignore it.
      sqlite3_clear_bindings(statement.statement);

      // Reset the rows read/written counters.
      sqlite3_stmt_status(statement.statement, LIBSQL_STMTSTATUS_ROWS_READ, 1);
      sqlite3_stmt_status(statement.statement, LIBSQL_STMTSTATUS_ROWS_WRITTEN, 1);
    }
  }
}

void SqliteDatabase::Query::checkRequirements(size_t size) {
  sqlite3_stmt* statement = getStatement();

  SQLITE_REQUIRE(!sqlite3_stmt_busy(statement), kj::none,
      "A SQL prepared statement can only be executed once at a time.");
  SQLITE_REQUIRE(size == sqlite3_bind_parameter_count(statement), kj::none,
      "Wrong number of parameter bindings for SQL query.");

  KJ_IF_SOME(cb, db.onWriteCallback) {
    if (!sqlite3_stmt_readonly(statement)) {
      cb();
    }
  }
}

void SqliteDatabase::Query::init(kj::ArrayPtr<const ValuePtr> bindings) {
  checkRequirements(bindings.size());

  for (auto i: kj::indices(bindings)) {
    bind(i, bindings[i]);
  }

  nextRow(/*first=*/true);
}

void SqliteDatabase::Query::bind(uint i, ValuePtr value) {
  sqlite3_stmt* statement = getStatement();

  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(blob, kj::ArrayPtr<const byte>) {
      SQLITE_CALL(sqlite3_bind_blob(statement, i + 1, blob.begin(), blob.size(), SQLITE_STATIC));
    }
    KJ_CASE_ONEOF(text, kj::StringPtr) {
      SQLITE_CALL(sqlite3_bind_text(statement, i + 1, text.begin(), text.size(), SQLITE_STATIC));
    }
    KJ_CASE_ONEOF(n, int64_t) {
      SQLITE_CALL(sqlite3_bind_int64(statement, i + 1, static_cast<long long>(n)));
    }
    KJ_CASE_ONEOF(x, double) {
      SQLITE_CALL(sqlite3_bind_double(statement, i + 1, x));
    }
    KJ_CASE_ONEOF(_, decltype(nullptr)) {
      SQLITE_CALL(sqlite3_bind_null(statement, i + 1));
    }
  }
}

uint64_t SqliteDatabase::Query::getRowsRead() {
  sqlite3_stmt* statement = getStatement();
  KJ_REQUIRE(statement != nullptr);
  return sqlite3_stmt_status(statement, LIBSQL_STMTSTATUS_ROWS_READ, 0);
}

uint64_t SqliteDatabase::Query::getRowsWritten() {
  sqlite3_stmt* statement = getStatement();
  return sqlite3_stmt_status(statement, LIBSQL_STMTSTATUS_ROWS_WRITTEN, 0);
}

void SqliteDatabase::Query::bind(uint i, kj::ArrayPtr<const byte> value) {
  sqlite3_stmt* statement = getStatement();
  SQLITE_CALL(sqlite3_bind_blob(statement, i + 1, value.begin(), value.size(), SQLITE_STATIC));
}

void SqliteDatabase::Query::bind(uint i, kj::StringPtr value) {
  sqlite3_stmt* statement = getStatement();
  SQLITE_CALL(sqlite3_bind_text(statement, i + 1, value.begin(), value.size(), SQLITE_STATIC));
}

void SqliteDatabase::Query::bind(uint i, long long value) {
  sqlite3_stmt* statement = getStatement();
  SQLITE_CALL(sqlite3_bind_int64(statement, i + 1, value));
}

void SqliteDatabase::Query::bind(uint i, double value) {
  sqlite3_stmt* statement = getStatement();
  SQLITE_CALL(sqlite3_bind_double(statement, i + 1, value));
}

void SqliteDatabase::Query::bind(uint i, decltype(nullptr)) {
  sqlite3_stmt* statement = getStatement();
  SQLITE_CALL(sqlite3_bind_null(statement, i + 1));
}

void SqliteDatabase::Query::nextRow(bool first) {
  auto& statementAndEffect = getStatementAndEffect();
  sqlite3_stmt* statement = statementAndEffect.statement;

  KJ_ASSERT(db.currentStatement == kj::none, "recursive nextRow()?");
  KJ_DEFER(db.currentStatement = kj::none);
  db.currentStatement = *statement;

  // The statement could be "re-prepared" during sqlite3_step, so we must set up the regulator.
  KJ_ASSERT(db.currentRegulator == kj::none, "nextRow() during prepare()?");
  KJ_DEFER(db.currentRegulator = kj::none);
  db.currentRegulator = regulator;

  SQLITE_CALL_SCOPE {
    int err = sqlite3_step(statement);
    // TODO(perf): This is slightly inefficient to call for every row read, but not bad enough to
    // fix it immediately. The alternate way would be to getRowsRead/Written once when we emit it
    // in the Dtor, and handle the case where the statement could be null when the Query gets
    // destructed
    rowsRead = getRowsRead();
    rowsWritten = getRowsWritten();
    if (err == SQLITE_DONE) {
      done = true;
    } else if (err != SQLITE_ROW) {
      SQLITE_CALL_FAILED("sqlite3_step()", err);
    }
  }

  if (first) {
    // A statement's effect is applied on the first step.
    db.applyChange(statementAndEffect.stateChange);
  }
}

uint SqliteDatabase::Query::changeCount() {
  KJ_REQUIRE(done);
  KJ_DREQUIRE(
      columnCount() == 0, "changeCount() can only be called on INSERT/UPDATE/DELETE queries");
  return sqlite3_changes(db);
}

uint SqliteDatabase::Query::columnCount() {
  sqlite3_stmt* statement = getStatement();
  return sqlite3_column_count(statement);
}

SqliteDatabase::Query::ValuePtr SqliteDatabase::Query::getValue(uint column) {
  sqlite3_stmt* statement = getStatement();
  switch (sqlite3_column_type(statement, column)) {
    case SQLITE_INTEGER:
      return getInt64(column);
    case SQLITE_FLOAT:
      return getDouble(column);
    case SQLITE_TEXT:
      return getText(column);
    case SQLITE_BLOB:
      return getBlob(column);
    case SQLITE_NULL:
      return nullptr;
  }
  KJ_UNREACHABLE;
}

kj::StringPtr SqliteDatabase::Query::getColumnName(uint column) {
  sqlite3_stmt* statement = getStatement();
  return sqlite3_column_name(statement, column);
}

kj::ArrayPtr<const byte> SqliteDatabase::Query::getBlob(uint column) {
  sqlite3_stmt* statement = getStatement();
  const byte* ptr = reinterpret_cast<const byte*>(sqlite3_column_blob(statement, column));
  return kj::arrayPtr(ptr, sqlite3_column_bytes(statement, column));
}

kj::StringPtr SqliteDatabase::Query::getText(uint column) {
  sqlite3_stmt* statement = getStatement();
  const char* ptr = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
  return kj::StringPtr(ptr, sqlite3_column_bytes(statement, column));
}

int SqliteDatabase::Query::getInt(uint column) {
  sqlite3_stmt* statement = getStatement();
  return sqlite3_column_int(statement, column);
}

int64_t SqliteDatabase::Query::getInt64(uint column) {
  sqlite3_stmt* statement = getStatement();
  return sqlite3_column_int64(statement, column);
}

double SqliteDatabase::Query::getDouble(uint column) {
  sqlite3_stmt* statement = getStatement();
  return sqlite3_column_double(statement, column);
}

bool SqliteDatabase::Query::isNull(uint column) {
  sqlite3_stmt* statement = getStatement();
  return sqlite3_column_type(statement, column) == SQLITE_NULL;
}

SqliteDatabase::StatementAndEffect& SqliteDatabase::Query::getStatementAndEffect() {
  return KJ_UNWRAP_OR(maybeStatement, {
    regulator.onError(kj::none, "SQLite query was canceled because the database was deleted.");
    KJ_FAIL_REQUIRE("query canceled because reset() was called on the database");
  });
}

void SqliteDatabase::Query::beforeSqliteReset() {
  // Note that if we don't own the statement, then `maybeStatement` is probably already dangling
  // here. Luckily, we don't need to reset it or anything because the statement will be destroyed
  // by Statement::beforeSqliteReset().
  maybeStatement = kj::none;
  ownStatement = {};
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
// We temporarily swap this for a real descriptor when our custom VFS wrapper is being invoked.

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

};  // namespace

// The sqlite3_file implementation we use when wrapping the native filesystem.
struct SqliteDatabase::Vfs::WrappedNativeFileImpl: public sqlite3_file {
  const Vfs* vfs;
  int rootFd;

  // It's expected that the wrapped sqlite_file begins in memory immediately after this object.
  sqlite3_file* getWrapped() {
    return reinterpret_cast<sqlite3_file*>(this + 1);
  }

  static const sqlite3_io_methods METHOD_TABLE;
};

// This completely nutso template generates wrapper functions for each of the function pointer
// members of sqlite3_vfs. The wrapper function temporarily sets `currentVfsRoot` to the FD
// of the directory from the SqliteDatabase::Vfs instance in use, then invokes the same function
// on the underlying native VFS.
template <typename Result,
    typename... Params,
    Result (*sqlite3_vfs::*slot)(sqlite3_vfs* vfs, Params...)>
struct SqliteDatabase::Vfs::MethodWrapperHack<Result (*sqlite3_vfs::*)(sqlite3_vfs* vfs, Params...),
    slot> {
  static Result wrapper(sqlite3_vfs* vfs, Params... params) noexcept {
    auto& self = *reinterpret_cast<SqliteDatabase::Vfs*>(vfs->pAppData);
    KJ_ASSERT(currentVfsRoot == AT_FDCWD);
    currentVfsRoot = self.rootFd;
    KJ_DEFER(currentVfsRoot = AT_FDCWD);
    return (self.native.*slot)(&self.native, params...);
  }
};

// Specialization of MethodWrapperHack for wrapping methods of sqlite_file, aka
// sqlite3_io_methods. Unfortunately, some file methods go back and perform filesystem ops. In
// particular, accessing shared memory associated with a file actually opens another adjacent
// file.
template <typename Result,
    typename... Params,
    Result (*sqlite3_io_methods::*slot)(sqlite3_file* file, Params...)>
struct SqliteDatabase::Vfs::
    MethodWrapperHack<Result (*sqlite3_io_methods::*)(sqlite3_file* file, Params...), slot> {
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

#define WRAP(name)                                                                                 \
  .name =                                                                                          \
      &MethodWrapperHack<decltype(&sqlite3_io_methods::name), &sqlite3_io_methods::name>::wrapper

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
  .xDeviceCharacteristics = [](sqlite3_file* file) noexcept -> int {
  auto wrapper = static_cast<WrappedNativeFileImpl*>(file);
  file = wrapper->getWrapped();
  KJ_ASSERT(currentVfsRoot == AT_FDCWD);
  currentVfsRoot = wrapper->rootFd;
  KJ_DEFER(currentVfsRoot = AT_FDCWD);
  return (file->pMethods->xDeviceCharacteristics)(file) |
      wrapper->vfs->options.deviceCharacteristics;
},

  WRAP(xShmMap),
  WRAP(xShmLock),
  WRAP(xShmBarrier),
  WRAP(xShmUnmap),

  WRAP(xFetch),
  WRAP(xUnfetch),
#undef WRAP
};

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
sqlite3_vfs SqliteDatabase::Vfs::makeWrappedNativeVfs() {
  static bool registerOnce KJ_UNUSED = ([&]() {
#define REPLACE_SYSCALL(name)                                                                      \
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

    .xOpen = [](sqlite3_vfs* vfs, sqlite3_filename zName, sqlite3_file* file, int flags,
                 int* pOutFlags) -> int {
    // We have to wrap xOpen explicitly because we need to further wrap each created file.
    //
    // My trick here is to prefix the native file with a second vtable. So the layout of the
    // `sqlite3_file` that we construct is actually a simple `struct sqlite_file` (which just
    // contains a single pointer to sqlite3_io_methods, i.e. the vtable pointer) _followed by_
    // the regular native file structure.

    auto wrapper = static_cast<WrappedNativeFileImpl*>(file);
    file = wrapper->getWrapped();
    file->pMethods = nullptr;

    // Set up currentVfsRoot.
    auto& self = *reinterpret_cast<const SqliteDatabase::Vfs*>(vfs->pAppData);
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
      wrapper->vfs = &self;
      wrapper->rootFd = self.rootFd;
    }

    return result;
  },

#define WRAP(name)                                                                                 \
  .name = &MethodWrapperHack<decltype(&sqlite3_vfs::name), &sqlite3_vfs::name>::wrapper

    WRAP(xDelete),
    WRAP(xAccess),
    .xFullPathname = [](sqlite3_vfs*, const char* zName, int nOut, char* zOut) -> int {
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
#endif  // #if !_WIN32

// -----------------------------------------------------------------------------
// Code to implement a true SQLite VFS based on `kj::Directory`.
//
// This VFS implementation actually delegates to the KJ filesystem interface for everything.
// This is used only when given a `kj::Directory` that does NOT represent a native file, i.e.
// one where `getFd()` returns null. This is mainly used for unit tests which want to use in-memory
// directories.

// Implementation of sqlite3_file.
//
// Weirdly, for sqlite3_file, SQLite uses a C++-like inheritance approach, with a separate
// virtual table that can be shared among all files of the same type. This is different from the
// way sqlite3_vfs works, where the function pointers are inlined into the sqlite3_vfs struct.
// In any case, as a result, `FileImpl`, unlike `VfsImpl`, is NOT just a namespace struct, but
// an actual instance.
struct SqliteDatabase::Vfs::FileImpl: public sqlite3_file {
  const Vfs& vfs;
  kj::Maybe<const kj::File&> writableFile;
  kj::Own<const kj::ReadableFile> file;

  kj::Maybe<kj::Own<Lock>> lock;
  // Rather complicatedly, SQLite doesn't consider the -shm file to be a separate file that it
  // opens via the VFS, but rather a facet of the database file itself. We implement it using an
  // entirely different interface anyway.
  //
  // We leave this null if the file is not the main database file.

  FileImpl(const Vfs& vfs, kj::Own<const kj::File> file, kj::Maybe<kj::Own<Lock>> lock)
      : sqlite3_file{.pMethods = &FILE_METHOD_TABLE},
        vfs(vfs),
        writableFile(*file),
        file(kj::mv(file)),
        lock(kj::mv(lock)) {}
  FileImpl(const Vfs& vfs, kj::Own<const kj::ReadableFile> file, kj::Maybe<kj::Own<Lock>> lock)
      : sqlite3_file{.pMethods = &FILE_METHOD_TABLE},
        vfs(vfs),
        file(kj::mv(file)),
        lock(kj::mv(lock)) {}

  static const sqlite3_io_methods FILE_METHOD_TABLE;
};

const sqlite3_io_methods SqliteDatabase::Vfs::FileImpl::FILE_METHOD_TABLE = {
  .iVersion = 3,
#define WRAP_METHOD(errorCode, block)                                                              \
  auto& self KJ_UNUSED = *static_cast<FileImpl*>(file);                                            \
  try block catch (kj::Exception & e) {                                                            \
    reportVfsErrorCaught(kj::mv(e));                                                               \
    return errorCode;                                                                              \
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
      bytes.slice(actual).fill(0);
      return SQLITE_IOERR_SHORT_READ;
    } else {
      return SQLITE_OK;
    }
  });
},

  .xWrite =
      [](sqlite3_file* file, const void* buffer, int iAmt, sqlite3_int64 iOfst) noexcept -> int {
  WRAP_METHOD(SQLITE_IOERR_WRITE, {
    KJ_IF_SOME(writableFile, self.writableFile) {
      auto bytes = kj::arrayPtr(reinterpret_cast<const byte*>(buffer), iAmt);
      writableFile.write(iOfst, bytes);
      return SQLITE_OK;
    } else {
      return SQLITE_READONLY;
    }
  });
},

  .xTruncate = [](sqlite3_file* file, sqlite3_int64 size) noexcept -> int {
  WRAP_METHOD(SQLITE_IOERR_TRUNCATE, {
    KJ_IF_SOME(writableFile, self.writableFile) {
      writableFile.truncate(size);
      return SQLITE_OK;
    } else {
      return SQLITE_READONLY;
    }
  });
},

  .xSync = [](sqlite3_file* file, int flags) noexcept -> int {
  WRAP_METHOD(SQLITE_IOERR_FSYNC, {
    if (flags & SQLITE_SYNC_DATAONLY) {
      self.file->datasync();
    } else {
      self.file->sync();
    }
    return SQLITE_OK;
  });
},

  .xFileSize = [](sqlite3_file* file, sqlite3_int64* pSize) noexcept -> int {
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
    auto& lock = *KJ_ASSERT_NONNULL(self.lock, "xLock called on file that isn't main database?");
    if (lock.tryIncreaseLevel(static_cast<Lock::Level>(level))) {
      return SQLITE_OK;
    } else {
      return SQLITE_BUSY;
    }
  });
},

  .xUnlock = [](sqlite3_file* file, int level) noexcept -> int {
  WRAP_METHOD(SQLITE_IOERR_UNLOCK, {
    auto& lock = *KJ_ASSERT_NONNULL(self.lock, "xLock called on file that isn't main database?");
    lock.decreaseLevel(static_cast<Lock::Level>(level));
    return SQLITE_OK;
  });
},

  .xCheckReservedLock = [](sqlite3_file* file, int* pResOut) noexcept -> int {
  WRAP_METHOD(SQLITE_IOERR_CHECKRESERVEDLOCK, {
    auto& lock = *KJ_ASSERT_NONNULL(self.lock, "xLock called on file that isn't main database?");
    *pResOut = lock.checkReservedLock();
    return SQLITE_OK;
  });
},

  .xFileControl = [](sqlite3_file* file, int op, void* pArg) noexcept -> int {
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
  WRAP_METHOD(SQLITE_IOERR, { return self.vfs.options.deviceCharacteristics; });
},

  .xShmMap =
      [](sqlite3_file* file, int iRegion, int szRegion, int bExtend, void volatile** pp) noexcept
  -> int {
  WRAP_METHOD(SQLITE_IOERR_SHMMAP, {
    KJ_ASSERT(iRegion >= 0);
    KJ_ASSERT(szRegion >= 0);
    auto& lock = *KJ_ASSERT_NONNULL(self.lock, "xShmMap called on file that isn't main database?");

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
    auto& lock = *KJ_ASSERT_NONNULL(self.lock, "xShmMap called on file that isn't main database?");
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
    auto& lock = *KJ_ASSERT_NONNULL(self.lock, "xShmMap called on file that isn't main database?");
    if (deleteFlag) {
      lock.clearSharedMemory();
    }
    return SQLITE_OK;  // return value is ignored by sqlite
  });
},

  .xFetch = [](sqlite3_file* file, sqlite3_int64 iOfst, int iAmt, void** pp) noexcept -> int {
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
  .xUnfetch = [](sqlite3_file* file, sqlite3_int64 iOfst, void* p) noexcept -> int {
  // Shouldn't ever be called since xFetch() always produces null? But the native implementation
  // return SQLITE_OK even when mmap is disabled so we will too.
  return SQLITE_OK;
},
#undef WRAP_METHOD
};

// SQLite VFS implementation based on abstract `kj::Directory`. This is used only when the
// directory is NOT a true disk directory.
//
// This is a namespace-struct defining static methods to fill in the function pointers of
// sqlite3_vfs.
sqlite3_vfs SqliteDatabase::Vfs::makeKjVfs() {
  return {
    .iVersion = kj::min(3, native.iVersion), .szOsFile = sizeof(FileImpl),

    .mxPathname = 512,
    // We have no real limit on paths but SQLite likes to allocate buffers of this size whenever
        // doing path stuff so making it huge would be bad. The default unix implementation uses
        // 512 as a limit so that "should be enough for anyone".

        .pNext = nullptr, .zName = name.cStr(), .pAppData = this,

#define WRAP_METHOD(errorCode, block)                                                              \
  auto& self KJ_UNUSED = *static_cast<const SqliteDatabase::Vfs*>(vfs->pAppData);                  \
  try block catch (kj::Exception & e) {                                                            \
    KJ_LOG(ERROR, "SQLite VFS I/O error", e);                                                      \
    return errorCode;                                                                              \
  }

    .xOpen = [](sqlite3_vfs* vfs, sqlite3_filename zName, sqlite3_file* file, int flags,
                 int* pOutFlags) -> int {
      WRAP_METHOD(SQLITE_CANTOPEN, {
        auto& target = *static_cast<FileImpl*>(file);

        if (flags & SQLITE_OPEN_READONLY) {
          KJ_REQUIRE(zName != nullptr, "readonly unnamed temporary file? what?");
          KJ_REQUIRE(!(flags & SQLITE_OPEN_CREATE), "create readonly file? what?");

          auto path = kj::Path::parse(zName);
          auto kjFile = KJ_UNWRAP_OR(self.directory.tryOpenFile(path), { return SQLITE_CANTOPEN; });
          kj::Maybe<kj::Own<Lock>> lock;
          if (flags & SQLITE_OPEN_MAIN_DB) {
            lock = self.lockManager.lock(path, *kjFile);
          }

          kj::ctor(target, self, kj::mv(kjFile), kj::mv(lock));
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
            kjFile =
                KJ_UNWRAP_OR(self.directory.tryOpenFile(path, mode), { return SQLITE_CANTOPEN; });
            if (flags & SQLITE_OPEN_MAIN_DB) {
              lock = self.lockManager.lock(path, *kjFile);
            }

            if (flags & SQLITE_OPEN_DELETEONCLOSE) {
              self.directory.remove(path);
            }
          }

          kj::ctor(target, self, kj::mv(kjFile), kj::mv(lock));
        }

        // In theory if read-write was requested, but failed, we should retry read-only, and then
        // alter the pOutFlags to reflect this... I'm not going to bother.
        if (pOutFlags != nullptr) {
          *pOutFlags = flags;
        }

        return SQLITE_OK;
      });
    },
    .xDelete = [](sqlite3_vfs* vfs, const char* zName, int syncDir) -> int {
      WRAP_METHOD(SQLITE_IOERR_DELETE, {
        if (self.directory.tryRemove(kj::Path::parse(zName))) {
          return SQLITE_OK;
        } else {
          return SQLITE_IOERR_DELETE_NOENT;
        }
      });
    },
    .xAccess = [](sqlite3_vfs* vfs, const char* zName, int flags, int* pResOut) -> int {
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
    .xFullPathname = [](sqlite3_vfs*, const char* zName, int nOut, char* zOut) -> int {
      // Don't rewrite the path at all. All paths are canonical. Our path parsing will reject
      // the existence of `.` or `..` as path components as well as leading `/`.
      size_t len = kj::min(strlen(zName), nOut - 1);
      memcpy(zOut, zName, len);
      zOut[len] = 0;
      return SQLITE_OK;
    },

    .xDlOpen = nullptr, .xDlError = nullptr, .xDlSym = nullptr, .xDlClose = nullptr,
    // We don't support loading shared libraries from virtual files.

        .xRandomness = native.xRandomness, .xSleep = native.xSleep,
    .xCurrentTime = native.xCurrentTime, .xGetLastError = nullptr,
    .xCurrentTimeInt64 = native.xCurrentTimeInt64,
    // Use native implementations of these OS functions. I'm not sure why these are even part
        // of the VFS. (Exception: xGetLastError is actually sensibly a VFS thing, but we are allowed
        // to just not implement it.)

        .xSetSystemCall = nullptr, .xGetSystemCall = nullptr, .xNextSystemCall = nullptr,
    // We don't support overriding any syscalls.

#undef WRAP_METHOD
  };
};

// -----------------------------------------------------------------------------

class SqliteDatabase::Vfs::DefaultLockManager final: public SqliteDatabase::LockManager {
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
    LockImpl(const DefaultLockManager& lockManager, kj::PathPtr path): lockManager(lockManager) {
      auto mlock = lockManager.lockMap.lockExclusive();
      auto& slot = mlock->findOrCreate(path, [&]() {
        state = kj::refcounted<LockState>(path.clone());
        return LockMap::Entry{.key = state->path, .value = state.get()};
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
          newRegion.asPtr().fill(0);
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

SqliteDatabase::Vfs::Vfs(const kj::Directory& directory, Options options)
    : directory(directory),
      ownLockManager(kj::heap<DefaultLockManager>()),
      lockManager(*ownLockManager),
      options(kj::mv(options)),
      native(*sqlite3_vfs_find(nullptr)) {
#if _WIN32
  vfs = kj::heap(makeKjVfs());
#else
  KJ_IF_SOME(fd, directory.getFd()) {
    rootFd = fd;
    vfs = kj::heap(makeWrappedNativeVfs());
  } else {
    vfs = kj::heap(makeKjVfs());
  }
#endif
  sqlite3_vfs_register(vfs, false);
}

SqliteDatabase::Vfs::Vfs(
    const kj::Directory& directory, const LockManager& lockManager, Options options)
    : directory(directory),
      lockManager(lockManager),
      options(kj::mv(options)),
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
  return kj::none;
}
#endif

// =======================================================================================

}  // namespace workerd
