// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite.h"
#include <cstdint>
#include <kj/test.h>
#include <kj/thread.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <atomic>

#if _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace workerd {
namespace {

// Initialize the database with some data.
void setupSql(SqliteDatabase& db) {
  // TODO(sqlite): Do this automatically and don't permit it via run().
  db.run("PRAGMA journal_mode=WAL;");

  {
    auto query = db.run(R"(
      CREATE TABLE people (
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL,
        email TEXT NOT NULL UNIQUE
      );

      INSERT INTO people (id, name, email)
      VALUES (?, ?, ?),
            (?, ?, ?);
    )", 123, "Bob"_kj, "bob@example.com"_kj,
        321, "Alice"_kj, "alice@example.com"_kj);

    KJ_EXPECT(query.changeCount() == 2);
  }
}

// Do some read-only queries on `db` to check that it's in the state that `setupSql()` ought to
// have left it in.
void checkSql(SqliteDatabase& db) {
  {
    auto query = db.run("SELECT * FROM people ORDER BY name");

    KJ_ASSERT(!query.isDone());
    KJ_ASSERT(query.columnCount() == 3);
    KJ_EXPECT(query.getInt(0) == 321);
    KJ_EXPECT(query.getText(1) == "Alice");
    KJ_EXPECT(query.getText(2) == "alice@example.com");

    query.nextRow();
    KJ_ASSERT(!query.isDone());
    KJ_EXPECT(query.getInt(0) == 123);
    KJ_EXPECT(query.getText(1) == "Bob");
    KJ_EXPECT(query.getText(2) == "bob@example.com");

    query.nextRow();
    KJ_EXPECT(query.isDone());
  }

  {
    auto query = db.run("SELECT * FROM people WHERE people.id = ?", 123l);

    KJ_ASSERT(!query.isDone());
    KJ_ASSERT(query.columnCount() == 3);
    KJ_EXPECT(query.getInt(0) == 123);
    KJ_EXPECT(query.getText(1) == "Bob");
    KJ_EXPECT(query.getText(2) == "bob@example.com");

    query.nextRow();
    KJ_EXPECT(query.isDone());
  }

  {
    auto query = db.run("SELECT * FROM people WHERE people.name = ?", "Alice"_kj);

    KJ_ASSERT(!query.isDone());
    KJ_ASSERT(query.columnCount() == 3);
    KJ_EXPECT(query.getInt(0) == 321);
    KJ_EXPECT(query.getText(1) == "Alice");
    KJ_EXPECT(query.getText(2) == "alice@example.com");

    query.nextRow();
    KJ_EXPECT(query.isDone());
  }
}

KJ_TEST("SQLite backed by in-memory directory") {
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);

  {
    SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

    setupSql(db);
    checkSql(db);

    {
      auto files = dir->listNames();
      KJ_ASSERT(files.size() == 2);
      KJ_EXPECT(files[0] == "foo");
      KJ_EXPECT(files[1] == "foo-wal");
    }
  }

  {
    auto files = dir->listNames();
    KJ_ASSERT(files.size() == 1);
    KJ_EXPECT(files[0] == "foo");
  }

  // Open it again and make sure tha data is still there!
  {
    SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::MODIFY);

    checkSql(db);
  }
}

class TempDirOnDisk {
public:
  TempDirOnDisk() {}
  ~TempDirOnDisk() noexcept(false) {
    dir = nullptr;
    disk->getRoot().remove(path);
  }

  const kj::Directory* operator->() {
    return dir;
  }
  const kj::Directory& operator*() {
    return *dir;
  }

private:
  kj::Own<kj::Filesystem> disk = kj::newDiskFilesystem();
  kj::Path path = makeTmpPath();
  kj::Own<const kj::Directory> dir = disk->getRoot().openSubdir(path, kj::WriteMode::MODIFY);

  kj::Path makeTmpPath() {
    const char* tmpDir = getenv("TEST_TMPDIR");
    kj::String pathStr = kj::str(
        tmpDir != nullptr ? tmpDir : "/var/tmp", "/workerd-sqlite-test.XXXXXX");
#if _WIN32
    if (_mktemp(pathStr.begin()) == nullptr) {
      KJ_FAIL_SYSCALL("_mktemp", errno, pathStr);
    }
    auto path = disk->getCurrentPath().evalNative(pathStr);
    disk->getRoot().openSubdir(path, kj::WriteMode::CREATE | kj::WriteMode::MODIFY |
                                     kj::WriteMode::CREATE_PARENT);
    return path;
#else
    if (mkdtemp(pathStr.begin()) == nullptr) {
      KJ_FAIL_SYSCALL("mkdtemp", errno, pathStr);
    }
    return disk->getCurrentPath().evalNative(pathStr);
#endif
  }
};

KJ_TEST("SQLite backed by real disk") {
  // Well, I made it possible to use an in-memory directory so that unit tests wouldn't have to
  // use real disk. But now I have to test that it does actually work on real disk. So here we are,
  // in a unit test, using real disk.

  TempDirOnDisk dir;
  SqliteDatabase::Vfs vfs(*dir);

  {
    SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

    setupSql(db);
    checkSql(db);

    {
      auto files = dir->listNames();
      KJ_ASSERT(files.size() == 3);
      KJ_EXPECT(files[0] == "foo");
      KJ_EXPECT(files[1] == "foo-shm");
      KJ_EXPECT(files[2] == "foo-wal");
    }
  }

  {
    auto files = dir->listNames();
    KJ_ASSERT(files.size() == 1);
    KJ_EXPECT(files[0] == "foo");
  }

  // Open it again and make sure tha data is still there!
  {
    SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::MODIFY);

    checkSql(db);
  }
}

// Tests that concurrent database clients don't clobber each other. This verifies that the
// LockManager interface is able to protect concurrent access and that our default implementation
// works.
void doLockTest(bool walMode) {
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);

  SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

  if (walMode) {
    db.run("PRAGMA journal_mode=WAL;");
  }

  db.run(R"(
    CREATE TABLE foo (
      id INTEGER PRIMARY KEY,
      counter INTEGER
    );

    INSERT INTO foo VALUES (0, 1)
  )");

  static constexpr char GET_COUNT[] = "SELECT counter FROM foo WHERE id = 0";
  static constexpr char INCREMENT[] = "UPDATE foo SET counter = counter + 1 WHERE id = 0";

  KJ_EXPECT(db.run(GET_COUNT).getInt(0) == 1);

  // Concurrent write allowed, as long as we're not writing at the same time.
  kj::Thread([&vfs = vfs]() noexcept {
    SqliteDatabase db2(vfs, kj::Path({"foo"}), kj::WriteMode::MODIFY);
    KJ_EXPECT(db2.run(GET_COUNT).getInt(0) == 1);
    db2.run(INCREMENT);
    KJ_EXPECT(db2.run(GET_COUNT).getInt(0) == 2);
  });

  KJ_EXPECT(db.run(GET_COUNT).getInt(0) == 2);

  std::atomic<bool> stop = false;
  std::atomic<uint> counter = 2;

  {
    // Arrange for two threads to increment in a loop simultaneously. Eventually one will fail with
    // a conflict.
    kj::Thread thread([&vfs = vfs, &stop, &counter]() noexcept {
      KJ_DEFER(stop.store(true, std::memory_order_relaxed););
      SqliteDatabase db2(vfs, kj::Path({"foo"}), kj::WriteMode::MODIFY);
      while (!stop.load(std::memory_order_relaxed)) {
        KJ_IF_SOME(e, kj::runCatchingExceptions([&]() {
          db2.run(INCREMENT);
          counter.fetch_add(1, std::memory_order_relaxed);
        })) {
          KJ_EXPECT(kj::_::hasSubstring(e.getDescription(), "database is locked"), e);
          break;
        }
      }
    });

    {
      KJ_DEFER(stop.store(true, std::memory_order_relaxed););

      while (!stop.load(std::memory_order_relaxed)) {
        KJ_IF_SOME(e, kj::runCatchingExceptions([&]() {
          db.run(INCREMENT);
          counter.fetch_add(1, std::memory_order_relaxed);
        })) {
          KJ_EXPECT(kj::_::hasSubstring(e.getDescription(), "database is locked"), e);
          break;
        }
      }
    }
  }

  // The final value should be consistent with the number of increments that succeeded.
  KJ_EXPECT(db.run(GET_COUNT).getInt(0) == counter.load(std::memory_order_relaxed));
}

KJ_TEST("SQLite locks: rollback journal mode") {
  doLockTest(false);
}

KJ_TEST("SQLite locks: WAL mode") {
  doLockTest(true);
}

KJ_TEST("SQLite Regulator") {
  TempDirOnDisk dir;
  SqliteDatabase::Vfs vfs(*dir);
  SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

  class RegulatorImpl: public SqliteDatabase::Regulator {
  public:
    RegulatorImpl(kj::StringPtr blocked): blocked(blocked) {}

    bool isAllowedName(kj::StringPtr name) override {
      if (alwaysFail) return false;
      return name != blocked;
    }

    bool alwaysFail = false;

  private:
    kj::StringPtr blocked;
  };

  db.run(R"(
    CREATE TABLE foo(value INTEGER);
    CREATE TABLE bar(value INTEGER);
    INSERT INTO foo VALUES (123);
    INSERT INTO bar VALUES (456);
  )");

  RegulatorImpl noFoo("foo");
  RegulatorImpl noBar("bar");

  // We can prepare and run statements that comply with the regulator.
  auto getFoo = db.prepare(noBar, "SELECT value FROM foo");
  auto getBar = db.prepare(noFoo, "SELECT value FROM bar");

  KJ_EXPECT(getFoo.run().getInt(0) == 123);
  KJ_EXPECT(getBar.run().getInt(0) == 456);

  // Trying to prepare a statement that violates the regulator fails.
  KJ_EXPECT_THROW_MESSAGE("access to foo.value is prohibited",
      db.prepare(noFoo, "SELECT value FROM foo"));

  // If we create a new table, all statements must be re-prepared, which re-runs the regulator.
  // Make sure that works.
  db.run("CREATE TABLE baz(value INTEGER)");

  KJ_EXPECT(getFoo.run().getInt(0) == 123);

  // Let's screw with SQLite and make the regulator fail on re-run to see what happens.
  noFoo.alwaysFail = true;
  KJ_EXPECT_THROW_MESSAGE("access to bar.value is prohibited",
      KJ_EXPECT(getBar.run().getInt(0) == 456));
}

KJ_TEST("SQLite onWrite callback") {
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);
  SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

  bool sawWrite = false;
  db.onWrite([&]() { sawWrite = true; });

  setupSql(db);
  KJ_EXPECT(sawWrite);
  sawWrite = false;

  checkSql(db);
  KJ_EXPECT(!sawWrite);  // checkSql() only does reads

  // Test for bug where the write callback would only be called for the last statement in a
  // multi-statement execution.
  auto q = db.run(R"(
    INSERT INTO people (id, name, email) VALUES (12321, "Eve", "eve@example.com");
    SELECT COUNT(*) FROM people;
  )");
  KJ_EXPECT(q.getInt(0) == 3);
  KJ_EXPECT(sawWrite);
}

struct RowCounts {
  uint64_t found;
  uint64_t read;
  uint64_t written;
};

template<typename ...Params>
RowCounts countRowsTouched(SqliteDatabase& db, SqliteDatabase::Regulator& regulator, kj::StringPtr sqlCode, Params... bindParams) {
  uint64_t rowsFound = 0;

  // Runs a query; retrieves and discards all the data.
  auto query = db.run(regulator, sqlCode, bindParams...);
  while (!query.isDone()) {
    rowsFound++;
    query.nextRow();
  }

  return {.found = rowsFound,
          .read = query.getRowsRead(),
          .written = query.getRowsWritten()};
}

template<typename ...Params>
RowCounts countRowsTouched(SqliteDatabase& db, kj::StringPtr sqlCode, Params... bindParams) {
  return countRowsTouched(db, SqliteDatabase::TRUSTED, sqlCode, std::forward<Params>(bindParams)...);
}

KJ_TEST("SQLite read row counters (basic)") {
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);
  SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

  db.run(R"(
    CREATE TABLE things (
      id INTEGER PRIMARY KEY,
      unindexed_int INTEGER,
      value TEXT
    );
  )");

  constexpr int dbRowCount = 1000;
  auto insertStmt = db.prepare("INSERT INTO things (id, unindexed_int, value) VALUES (?, ?, ?)");
  for (int i = 0; i < dbRowCount; i++) {
    insertStmt.run(i, i * 1000, kj::str("value", i));
  }

  // Sanity check that the inserts worked.
  {
    auto getCount = db.prepare("SELECT COUNT(*) FROM things");
    KJ_EXPECT(getCount.run().getInt(0) == dbRowCount);
  }

  // Selecting all the rows reads all the rows.
  {
    RowCounts stats = countRowsTouched(db, "SELECT * FROM things");
    KJ_EXPECT(stats.found == dbRowCount);
    KJ_EXPECT(stats.read == dbRowCount);
    KJ_EXPECT(stats.written == 0);
  }

  // Selecting one row using an index reads one row.
  {
    RowCounts stats = countRowsTouched(db, "SELECT * FROM things WHERE id=?", 5);
    KJ_EXPECT(stats.found == 1);
    KJ_EXPECT(stats.read == 1);
    KJ_EXPECT(stats.written == 0);
  }

  // Selecting one row using an reads one row, even if that row is in the middle of the table.
  {
    RowCounts stats = countRowsTouched(db, "SELECT * FROM things WHERE id=?", dbRowCount / 2);
    KJ_EXPECT(stats.found == 1);
    KJ_EXPECT(stats.read == 1);
    KJ_EXPECT(stats.written == 0);
  }

  // Selecting a row by an unindexed value reads the whole table.
  {
    RowCounts stats = countRowsTouched(db, "SELECT * FROM things WHERE unindexed_int = ?", 5000);
    KJ_EXPECT(stats.found == 1);
    KJ_EXPECT(stats.read == dbRowCount);
    KJ_EXPECT(stats.written == 0);
  }

  // Selecting an unindexed aggregate scans all the rows, which counts as reading them.
  {
    RowCounts stats = countRowsTouched(db, "SELECT MAX(unindexed_int) FROM things");
    KJ_EXPECT(stats.found == 1);
    KJ_EXPECT(stats.read == dbRowCount);
    KJ_EXPECT(stats.written == 0);
  }

  // Selecting an indexed aggregate can use the index, so it only reads the row it found.
  {
    RowCounts stats = countRowsTouched(db, "SELECT MIN(id) FROM things");
    KJ_EXPECT(stats.found == 1);
    KJ_EXPECT(stats.read == 1);
    KJ_EXPECT(stats.written == 0);
  }

  // Selecting with a limit only reads the returned rows.
  {
    RowCounts stats = countRowsTouched(db, "SELECT * FROM things LIMIT 5");
    KJ_EXPECT(stats.found == 5);
    KJ_EXPECT(stats.read == 5);
    KJ_EXPECT(stats.written == 0);
  }
}

KJ_TEST("SQLite write row counters (basic)") {
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);
  SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

  db.run(R"(
    CREATE TABLE things (
      id INTEGER PRIMARY KEY
    );
  )");

  db.run(R"(
    CREATE TABLE unindexed_things (
      id INTEGER
    );
  )");

  // Inserting a row counts as one row written.
  {
    RowCounts stats = countRowsTouched(db, "INSERT INTO unindexed_things (id) VALUES (?)", 1);
    KJ_EXPECT(stats.read == 0);
    KJ_EXPECT(stats.written == 1);
  }

  // Inserting a row into a table with a primary key will also do a read (to ensure there's no
  // duplicate PK).
  {
    RowCounts stats = countRowsTouched(db, "INSERT INTO things (id) VALUES (?)", 1);
    KJ_EXPECT(stats.read == 1);
    KJ_EXPECT(stats.written == 1);
  }

  // Deleting a row counts as a write.
  {
    RowCounts stats = countRowsTouched(db, "INSERT INTO things (id) VALUES (?)", 123);
    KJ_EXPECT(stats.written == 1);

    stats = countRowsTouched(db, "DELETE FROM things WHERE id=?", 123);
    KJ_EXPECT(stats.read == 1);
    KJ_EXPECT(stats.written == 1);
  }

  // Deleting nothing is not a write.
  {
    RowCounts stats = countRowsTouched(db, "DELETE FROM things WHERE id=?", 998877112233);
    KJ_EXPECT(stats.written == 0);
  }

  // Inserting many things is many writes.
  {
    db.run("DELETE FROM things");
    db.run("INSERT INTO things (id) VALUES (1)");
    db.run("INSERT INTO things (id) VALUES (3)");
    db.run("INSERT INTO things (id) VALUES (5)");

    RowCounts stats = countRowsTouched(db, "INSERT INTO unindexed_things (id) SELECT id FROM things");
    KJ_EXPECT(stats.read == 3);
    KJ_EXPECT(stats.written == 3);
  }

  // Each updated row is a write.
  {
    db.run("DELETE FROM unindexed_things");
    db.run("INSERT INTO unindexed_things (id) VALUES (1)");
    db.run("INSERT INTO unindexed_things (id) VALUES (2)");
    db.run("INSERT INTO unindexed_things (id) VALUES (3)");
    db.run("INSERT INTO unindexed_things (id) VALUES (4)");

    RowCounts stats = countRowsTouched(db, "UPDATE unindexed_things SET id = id * 10 WHERE id >= 3");
    KJ_EXPECT(stats.written == 2);
  }

  // On an indexed table, each updated row is two writes. This is probably due to the index update.
  {
    db.run("DELETE FROM things");
    db.run("INSERT INTO things (id) VALUES (1)");
    db.run("INSERT INTO things (id) VALUES (2)");
    db.run("INSERT INTO things (id) VALUES (3)");
    db.run("INSERT INTO things (id) VALUES (4)");

    RowCounts stats = countRowsTouched(db, "UPDATE things SET id = id * 10 WHERE id >= 3");
    KJ_EXPECT(stats.read >= 4);  // At least one read per updated row
    KJ_EXPECT(stats.written == 4);
  }
}

KJ_TEST("SQLite row counters with triggers") {
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);
  SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

  class RegulatorImpl: public SqliteDatabase::Regulator {
  public:
    RegulatorImpl() = default;

    bool isAllowedTrigger(kj::StringPtr name) override {
      // SqliteDatabase::TRUSTED doesn't let us use triggers at all.
      return true;
    }
  };

  RegulatorImpl regulator;

  db.run(R"(
    CREATE TABLE things (
      id INTEGER PRIMARY KEY
    );

    CREATE TABLE log (
      id INTEGER,
      verb TEXT
    );

    CREATE TRIGGER log_inserts AFTER INSERT ON things
    BEGIN
      insert into log (id, verb) VALUES (NEW.id, "INSERT");
    END;

    CREATE TRIGGER log_deletes AFTER DELETE ON things
    BEGIN
      insert into log (id, verb) VALUES (OLD.id, "DELETE");
    END;
  )");

  // Each insert incurs two writes: one for the row in `things` and one for the row in `log`.
  {
    RowCounts stats = countRowsTouched(db, regulator, "INSERT INTO things (id) VALUES (1)");
    KJ_EXPECT(stats.written == 2);
  }

  // A deletion incurs two writes: one for the row and one for the log.
  {
    db.run(regulator, "DELETE FROM things");
    db.run(regulator, "INSERT INTO things (id) VALUES (1)");
    db.run(regulator, "INSERT INTO things (id) VALUES (2)");
    db.run(regulator, "INSERT INTO things (id) VALUES (3)");

    RowCounts stats = countRowsTouched(db, regulator, "DELETE FROM things");
    KJ_EXPECT(stats.written == 6);
  }
}

KJ_TEST("DELETE with LIMIT") {
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);
  SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

  db.run(R"(
    CREATE TABLE things (
      id INTEGER PRIMARY KEY
    );
  )");

  db.run(R"(INSERT INTO things (id) VALUES (1))");
  db.run(R"(INSERT INTO things (id) VALUES (2))");
  db.run(R"(INSERT INTO things (id) VALUES (3))");
  db.run(R"(INSERT INTO things (id) VALUES (4))");
  db.run(R"(INSERT INTO things (id) VALUES (5))");
  db.run(R"(DELETE FROM things LIMIT 2)");
  auto q = db.run(R"(SELECT COUNT(*) FROM things;)");
  KJ_EXPECT(q.getInt(0) == 3);
}

}  // namespace
}  // namespace workerd
