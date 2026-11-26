// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The capture against real `SQLite`, through workerd's `SqliteDatabase`
//! (`sqlite-rs`): the application's databases are written through a VFS
//! that reports WAL writes to a `Capture`, as a clustered namespace's
//! storage reports them to the cluster, and whatever the capture cut must
//! restore to what the application sees.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::path::Path;
use std::path::PathBuf;
use std::rc::Rc;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use sqlite_rs::Database;
use sqlite_rs::OpenMode;
use sqlite_rs::Vfs;
use sqlite_rs::VfsOptions;
use sqlite_rs::WalObserver;

use crate::Bucket;
use crate::Error;
use crate::Sequence;
use crate::capture::Capture;
use crate::log::DB;
use crate::log::File as LogFile;
use crate::log::Log;
use crate::log::restore;
use crate::owner::Layout;
use crate::owner::Owners;
use crate::ownership::Claim;
use crate::ownership::Ownership;
use crate::registry::Registry;

/// Just enough of `SQLite` for the tests: run SQL, read one row of integers.
struct Connection(Database);

impl Connection {
    /// A connection to `path` through a VFS of its own, which no capture
    /// watches.
    fn open(path: &Path) -> Self {
        let vfs = Vfs::on_disk(path.parent().unwrap().to_str().unwrap()).unwrap();
        Self::open_in(&vfs, path.file_name().unwrap().to_str().unwrap())
    }

    /// A connection to `path` through the capturing VFS of `open_app`, as
    /// a clustered namespace's storage opens its databases.
    fn capturing(path: &Path) -> Self {
        CAPTURING.with_borrow(|capturing| {
            let (dir, vfs) = capturing.as_ref().unwrap();
            let name = path.strip_prefix(dir).unwrap().to_str().unwrap();
            Self::open_in(vfs, name)
        })
    }

    fn open_in(vfs: &Vfs, name: &str) -> Self {
        Self(Database::open(vfs, name, OpenMode::Create).unwrap())
    }

    fn execute(&self, sql: &str) {
        let rows = self.0.run(sql, &[]);
        rows.and_then(sqlite_rs::Rows::finish)
            .unwrap_or_else(|e| panic!("{sql:?}: {e:?}"));
    }

    fn row(&self, sql: &str, columns: u32) -> Vec<i64> {
        let rows = self.0.run(sql, &[]).unwrap();
        assert!(!rows.is_done(), "{sql:?}");
        (0..columns).map(|c| rows.i64(c).unwrap()).collect()
    }
}

thread_local! {
    static CAPTURE: RefCell<Option<Capture>> = const { RefCell::new(None) };
    /// The directory `open_app` opened the application in, and the VFS over
    /// it that reports WAL writes to `CAPTURE`.
    static CAPTURING: RefCell<Option<(PathBuf, Vfs)>> = const { RefCell::new(None) };
}

/// Runs a test's `body` under `sqlite_rs_testing::checked`, which fails it
/// when anything is logged at ERROR, such as an allocation outside a
/// database's memory scope. The capture and its VFS are dropped before the
/// check ends, not when the test's thread exits.
fn checked(body: impl FnOnce()) {
    sqlite_rs_testing::checked(|| {
        body();
        CAPTURE.take();
        CAPTURING.take();
    });
}

/// Passes each WAL write on to `CAPTURE`, with the database's file name, as
/// `Cluster::wal_write` does.
struct ToCapture;

impl WalObserver for ToCapture {
    fn wal_write(&self, path: &str, offset: u64, data: &[u8]) {
        let db = Path::new(path.strip_suffix("-wal").unwrap());
        let db = db.file_name().unwrap().to_str().unwrap();
        CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().write(db, offset, data));
    }
}

/// A new directory for `name`: numbered, as tests that run at once may ask
/// for the same name.
fn scratch(name: &str) -> PathBuf {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let n = NEXT.fetch_add(1, Ordering::Relaxed);
    let root = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let dir = root.join(format!("capture-{name}-{}-{n}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// The application's connection, through the capturing VFS, with the
/// capture already watching.
fn open_app(path: &Path) -> Connection {
    open_app_with(path, "")
}

/// `open_app`, running `pragmas` before the database is created.
fn open_app_with(path: &Path, pragmas: &str) -> Connection {
    let dir = path.parent().unwrap();
    let vfs = Vfs::on_disk_observed(dir.to_str().unwrap(), VfsOptions::new(), ToCapture).unwrap();
    CAPTURING.set(Some((dir.to_owned(), vfs)));
    CAPTURE.set(Some(Capture::open(path).unwrap()));
    let app = Connection::capturing(path);
    app.execute(&format!(
        "{pragmas} PRAGMA journal_mode=WAL; CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)"
    ));
    app
}

fn insert(app: &Connection, from: i64, to: i64) {
    let mut sql = String::from("BEGIN;");
    for id in from..to {
        sql += &format!(
            "INSERT INTO t (id, v) VALUES ({id}, '{}');",
            "x".repeat(200)
        );
    }
    sql += "COMMIT;";
    app.execute(&sql);
}

/// The table as one comparable value: row count, id sum, and value bytes.
fn rows(conn: &Connection) -> (i64, i64, i64) {
    let r = conn.row(
        "SELECT count(*), coalesce(sum(id), 0), coalesce(sum(length(v)), 0) FROM t",
        3,
    );
    (r[0], r[1], r[2])
}

fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}

/// The frame after the capture's last commit.
fn last() -> Sequence {
    CAPTURE.with_borrow(|c| c.as_ref().unwrap().last().unwrap())
}

/// The captured frames `from..to`, merged.
fn merged(from: Sequence, to: Sequence) -> Vec<u8> {
    let bytes = CAPTURE.with_borrow(|c| c.as_ref().unwrap().batch(from, to).unwrap());
    crate::log::merge(&bytes).unwrap()
}

/// The captured frames `from..to` as one batch in `dir`.
fn batch(dir: &Path, from: Sequence, to: Sequence) -> (LogFile, PathBuf) {
    let bytes = merged(from, to);
    let path = dir.join(format!("{from}-{to}"));
    std::fs::write(&path, crate::log::compress(&bytes).unwrap()).unwrap();
    let file = LogFile {
        snapshot: false,
        session: 0,
        frame: to,
        size: 0,
    };
    (file, path)
}

fn log(store: &Bucket) -> Log {
    Log::new(store.at("cell"))
}

/// Upload the captured frames `from..to` as one batch, as a commit does.
fn upload(store: &Bucket, from: Sequence, to: Sequence) {
    let dir = scratch(&format!("upload-{from}-{to}"));
    let (file, path) = batch(&dir, from, to);
    runtime().block_on(log(store).put(&file, &path)).unwrap();
}

/// Upload a snapshot at `to` of the captured frames, as compaction does.
fn upload_snapshot(store: &Bucket, to: Sequence) {
    let dir = scratch(&format!("snapshot-{to}"));
    let files = [batch(&dir, 0, to)];
    let snapshot = crate::log::compact(&files, &dir).unwrap();
    let path = dir.join(snapshot.name());
    runtime()
        .block_on(log(store).put(&snapshot, &path))
        .unwrap();
}

/// Restores the log's lanes into a spool, moving the root's to `out`.
fn restore_db(store: &Bucket, out: &Path) -> crate::Result<Sequence> {
    let spool = crate::log::with_suffix(out, ".spool");
    let (end, lanes) = runtime().block_on(restore(&log(store), 0, &spool))?;
    for (lane, path) in lanes {
        if lane == DB {
            std::fs::rename(path, out)?;
        }
    }
    Ok(end)
}

fn restored_rows(store: &Bucket, out: &Path) -> (i64, i64, i64) {
    restore_db(store, out).unwrap();
    rows(&Connection::open(out))
}

#[test]
fn restore_reproduces_the_application_rows() {
    checked(|| {
        let dir = scratch("roundtrip");
        let app = open_app(&dir.join("app.db"));
        let store = Bucket::memory("");
        insert(&app, 0, 100);
        let shipped = last();
        upload(&store, 0, shipped);
        insert(&app, 100, 250);
        app.execute("DELETE FROM t WHERE id % 7 = 0");
        upload(&store, shipped, last());
        let want = rows(&app);
        assert_eq!(want.0, 250 - 36);
        assert_eq!(restored_rows(&store, &dir.join("restored.db")), want);
    });
}

#[test]
fn restore_reads_the_newest_snapshot_and_what_extends_it() {
    checked(|| {
        let dir = scratch("snapshot");
        let app = open_app(&dir.join("app.db"));
        let store = Bucket::memory("");
        insert(&app, 0, 100);
        let a = last();
        upload(&store, 0, a);
        upload_snapshot(&store, a);
        insert(&app, 100, 200);
        let b = last();
        upload(&store, a, b);
        upload_snapshot(&store, b);
        app.execute("DELETE FROM t WHERE id % 3 = 0");
        upload(&store, b, last());
        let files = runtime().block_on(log(&store).files(0)).unwrap();
        let plan: Vec<_> = crate::log::plan(&files)
            .iter()
            .map(|f| (f.snapshot, f.frame))
            .collect();
        assert_eq!(plan, [(true, b), (false, last())]);
        assert_eq!(restored_rows(&store, &dir.join("restored.db")), rows(&app));
    });
}

#[test]
fn a_snapshot_inside_a_wal_carries_it_across_checkpoints() {
    checked(|| {
        let dir = scratch("snapshot-wal");
        let app = open_app(&dir.join("app.db"));
        let store = Bucket::memory("");
        app.execute("PRAGMA wal_autocheckpoint=8");
        for from in (0..400).step_by(10) {
            insert(&app, from, from + 10);
        }
        let a = last();
        upload_snapshot(&store, a);
        for from in (400..800).step_by(10) {
            insert(&app, from, from + 10);
        }
        upload(&store, a, last());
        assert_eq!(restored_rows(&store, &dir.join("restored.db")), rows(&app));
    });
}

#[test]
fn a_snapshot_compacts_from_the_last_one_inside_a_wal() {
    checked(|| {
        // No checkpoint: the second snapshot checkpoints a WAL that continues
        // past the first, from a frame, not a WAL header.
        let dir = scratch("resnapshot");
        let app = open_app(&dir.join("app.db"));
        let store = Bucket::memory("");
        insert(&app, 0, 100);
        let a = last();
        let files = [batch(&dir, 0, a)];
        let first = crate::log::compact(&files, &dir).unwrap();
        insert(&app, 100, 200);
        let b = last();
        let files = [(first, dir.join(first.name())), batch(&dir, a, b)];
        let second = crate::log::compact(&files, &dir).unwrap();
        assert_eq!(second.frame, b);
        runtime()
            .block_on(log(&store).put(&second, &dir.join(second.name())))
            .unwrap();
        insert(&app, 200, 300);
        upload(&store, b, last());
        assert_eq!(restored_rows(&store, &dir.join("restored.db")), rows(&app));
    });
}

#[test]
fn a_batch_keeps_one_copy_of_a_page_its_commits_rewrote() {
    checked(|| {
        let dir = scratch("dedup");
        let app = open_app(&dir.join("app.db"));
        let store = Bucket::memory("");
        insert(&app, 0, 10);
        let a = last();
        for i in 0..100 {
            app.execute(&format!("UPDATE t SET v = '{i}' WHERE id = 1"));
        }
        let raw = CAPTURE.with_borrow(|c| c.as_ref().unwrap().batch(a, last()).unwrap());
        let merged = merged(a, last());
        assert!(
            merged.len() * 20 < raw.len(),
            "{} of {}",
            merged.len(),
            raw.len()
        );
        upload(&store, 0, a);
        upload(&store, a, last());
        assert_eq!(restored_rows(&store, &dir.join("restored.db")), rows(&app));
    });
}

#[test]
fn a_corrupt_file_fails_to_restore() {
    checked(|| {
        let dir = scratch("corrupt");
        let app = open_app(&dir.join("app.db"));
        let store = Bucket::memory("");
        insert(&app, 0, 100);
        let (file, path) = batch(&dir, 0, last());
        let mut bytes = std::fs::read(&path).unwrap();
        let mid = bytes.len() / 2;
        bytes[mid] ^= 1;
        std::fs::write(&path, bytes).unwrap();
        runtime().block_on(log(&store).put(&file, &path)).unwrap();
        let restored = restore_db(&store, &dir.join("restored.db"));
        assert!(restored.is_err(), "{restored:?}");
    });
}

#[test]
fn a_truncated_file_fails_to_restore() {
    checked(|| {
        // Cut anywhere, including before the trailer that holds the checksum,
        // or empty.
        let dir = scratch("truncated");
        let app = open_app(&dir.join("app.db"));
        insert(&app, 0, 100);
        let (file, path) = batch(&dir, 0, last());
        let bytes = std::fs::read(&path).unwrap();
        let len = bytes.len();
        for cut in [&bytes[..len - 1], &bytes[..len - 8], &bytes[..len / 2], &[]] {
            let store = Bucket::memory("");
            std::fs::write(&path, cut).unwrap();
            runtime().block_on(log(&store).put(&file, &path)).unwrap();
            let out = dir.join("restored.db");
            let _ = std::fs::remove_file(&out);
            let restored = restore_db(&store, &out);
            assert!(restored.is_err(), "{restored:?}");
        }
    });
}

#[test]
fn a_rolled_back_transaction_leaves_nothing() {
    checked(|| {
        let dir = scratch("rollback");
        let app = open_app(&dir.join("app.db"));
        let store = Bucket::memory("");
        insert(&app, 0, 10);
        let wal = || std::fs::metadata(dir.join("app.db-wal")).unwrap().len();
        let before = wal();
        // A small cache spills the doomed transaction's frames into the WAL.
        // (`SqliteDatabase` refuses a recursive query, so the rows are inserted
        // a batch at a time.)
        app.execute("PRAGMA cache_size=2; BEGIN;");
        for from in (1000..=3000).step_by(500) {
            let mut sql = String::new();
            for id in from..(from + 500).min(3001) {
                sql += &format!("INSERT INTO t VALUES ({id}, hex(randomblob(100)));");
            }
            app.execute(&sql);
        }
        assert!(wal() > before + 100 * 4096, "no frames spilled");
        app.execute("ROLLBACK;");
        insert(&app, 10, 20);
        upload(&store, 0, last());
        assert_eq!(rows(&app).0, 20);
        assert_eq!(restored_rows(&store, &dir.join("restored.db")), rows(&app));
    });
}

#[test]
fn checkpoints_and_a_restart_keep_the_log_whole() {
    checked(|| {
        let dir = scratch("checkpoint");
        // `SqliteDatabase` refuses VACUUM, which attaches a database, so the
        // commits that shrink the database are auto-vacuum's.
        let app = open_app_with(&dir.join("app.db"), "PRAGMA auto_vacuum=FULL;");
        let store = Bucket::memory("");
        // Frequent checkpoints: the WAL starts over many times.
        app.execute("PRAGMA wal_autocheckpoint=8");
        for from in (0..600).step_by(50) {
            insert(&app, from, from + 50);
        }
        app.execute("PRAGMA wal_checkpoint(TRUNCATE)");
        insert(&app, 600, 650);
        upload(&store, 0, last());
        assert_eq!(restored_rows(&store, &dir.join("one.db")), rows(&app));

        // A new log from the live database, then more commits, shrinking at
        // the end.
        app.execute("PRAGMA wal_checkpoint(TRUNCATE)");
        CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().restart().unwrap());
        insert(&app, 650, 700);
        let pages = || app.row("PRAGMA page_count", 1)[0];
        let before = pages();
        app.execute("DELETE FROM t WHERE id > 20");
        assert!(pages() < before / 4, "{} of {before} pages", pages());
        insert(&app, 5000, 5010);
        let store = Bucket::memory("");
        upload(&store, 0, last());
        assert_eq!(rows(&app).0, 31);
        assert_eq!(restored_rows(&store, &dir.join("two.db")), rows(&app));
    });
}

#[test]
fn a_restart_after_the_database_closed_reads_the_file() {
    checked(|| {
        let dir = scratch("closed");
        let app = open_app(&dir.join("app.db"));
        insert(&app, 0, 50);
        let want = rows(&app);
        // Closing the last connection checkpoints the WAL and deletes it, so
        // the committed pages the capture knows of are in the database file.
        drop(app);
        assert!(!dir.join("app.db-wal").exists());
        CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().restart().unwrap());
        // Reopened, before its first write: an empty WAL.
        let app = Connection::capturing(&dir.join("app.db"));
        assert_eq!(rows(&app), want);
        assert_eq!(std::fs::metadata(dir.join("app.db-wal")).unwrap().len(), 0);
        CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().restart().unwrap());
        let store = Bucket::memory("");
        upload(&store, 0, last());
        assert_eq!(restored_rows(&store, &dir.join("restored.db")), want);
    });
}

#[test]
fn a_batch_the_log_already_covers_is_not_applied() {
    checked(|| {
        // Tiering and a bucket-acknowledged commit upload concurrently, so a
        // stale range can land beside a newer, wider one (lost writes on the
        // fleet): the third batch is inside the second, and must not override
        // it.
        let dir = scratch("covered");
        let app = open_app(&dir.join("app.db"));
        let store = Bucket::memory("");
        let mut ends = Vec::new();
        for from in (0..120).step_by(10) {
            insert(&app, from, from + 10);
            ends.push(last());
        }
        upload(&store, 0, ends[0]);
        upload(&store, ends[0], ends[11]);
        upload(&store, ends[1], ends[7]);
        assert_eq!(restored_rows(&store, &dir.join("restored.db")), rows(&app));
    });
}

#[test]
fn a_missing_batch_fails_to_restore_or_compact() {
    checked(|| {
        // The batch after a lost one starts past the frame the log reached,
        // and no wider one covers it.
        let dir = scratch("missing");
        let app = open_app(&dir.join("app.db"));
        let store = Bucket::memory("");
        let mut ends = Vec::new();
        for from in (0..30).step_by(10) {
            insert(&app, from, from + 10);
            ends.push(last());
        }
        upload(&store, 0, ends[0]);
        upload(&store, ends[1], ends[2]);
        let restored = restore_db(&store, &dir.join("restored.db"));
        assert!(restored.is_err(), "{restored:?}");
        // Nor does compaction snapshot it, then delete the batches.
        let files = [batch(&dir, 0, ends[0]), batch(&dir, ends[1], ends[2])];
        let compacted = crate::log::compact(&files, &dir);
        assert!(compacted.is_err(), "{compacted:?}");
    });
}

#[test]
fn a_deleted_database_restores_empty_then_as_recreated() {
    checked(|| {
        // A facet's delete() or a deleteAll() removes the file; the new
        // database's first page then reaches the file, not the WAL.
        let dir = scratch("reset");
        let path = dir.join("app.db");
        let app = open_app(&path);
        insert(&app, 0, 50);
        drop(app);
        std::fs::remove_file(&path).unwrap();
        CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().reset("app.db"));
        let deleted = last();
        let store = Bucket::memory("");
        upload(&store, 0, deleted);
        let out = dir.join("empty.db");
        restore_db(&store, &out).unwrap();
        assert!(!out.exists());

        let app = Connection::capturing(&path);
        app.execute("PRAGMA journal_mode=WAL; CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
        insert(&app, 100, 110);
        upload(&store, deleted, last());
        assert_eq!(rows(&app).0, 10);
        assert_eq!(restored_rows(&store, &dir.join("split.db")), rows(&app));
        // As one batch.
        let store = Bucket::memory("");
        upload(&store, 0, last());
        assert_eq!(restored_rows(&store, &dir.join("merged.db")), rows(&app));
    });
}

#[test]
fn a_superseded_owners_commit_is_not_acknowledged() {
    checked(|| {
        // a owns x and commits while b takes x over: the upload lands in a's
        // epoch, but own.json no longer names it, so a must not acknowledge.
        let dir = scratch("superseded");
        let path = dir.join("x.db");
        let store = Bucket::memory("").at("cluster");
        let node = |name: &str| {
            let registry = Rc::new(Registry::new(store.clone(), name, name, 10_000));
            let ownership = Ownership::new(store.clone(), registry.clone());
            (registry, Owners::new(store.clone()), ownership)
        };
        let ((a, a_owners, a_own), (b, b_owners, b_own)) = (node("a"), node("b"));
        let claim =
            |own: &Ownership, now| match runtime().block_on(own.acquire_or_route("ns", "x", now)) {
                Ok(Claim::Owned(owned)) => owned,
                other => panic!("{other:?}"),
            };
        runtime().block_on(a.renew(0)).unwrap();
        let owned = claim(&a_own, 0);
        let rt = runtime();
        assert!(
            rt.block_on(a_owners.open_fresh(&a_own, "ns", "x", &owned))
                .unwrap()
        );
        rt.block_on(a_owners.take_over(&a_own, "ns", "x", &owned, &Root(path.clone())))
            .unwrap();

        let app = Connection::open(&path);
        app.execute("PRAGMA journal_mode=WAL; CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
        insert(&app, 0, 10);
        let wal = std::fs::read(dir.join("x.db-wal")).unwrap();
        a_owners.wal_write("ns", "x", "x.db", 0, &wal[..32]);
        a_owners.wal_write("ns", "x", "x.db", 32, &wal[32..]);

        // a stops renewing; b takes x over at epoch 2.
        rt.block_on(b.renew(9_000)).unwrap();
        let owned = claim(&b_own, 11_000);
        assert_eq!(owned.epoch, 2);
        let b_path = Root(dir.join("b.db"));
        rt.block_on(b_owners.take_over(&b_own, "ns", "x", &owned, &b_path))
            .unwrap();

        let commit = rt.block_on(a_owners.commit(&a_own, "ns", "x"));
        assert!(matches!(&commit, Err(Error::Replica(_))), "{commit:?}");
        assert_eq!(
            commit.unwrap_err().to_string(),
            "replica: ns/x: epoch 1 was superseded"
        );

        // b's epoch is open, so no restore reads a's session: b deletes it,
        // with the upload a made after b took over.
        let x = format!("{}{}", store.prefix(), crate::object_prefix("ns", "x"));
        let x = Log::new(store.at(&x));
        assert!(!rt.block_on(x.older(2)).unwrap().is_empty());
        rt.block_on(b_owners.tier());
        assert!(rt.block_on(x.older(2)).unwrap().is_empty());
    });
}

#[test]
fn a_compaction_keeps_an_object_busy_once_it_began() {
    checked(|| {
        // a's compaction of x waits for x's lock, which an upload holds, while x
        // stops being served here: x stays busy until the compaction ends, so a
        // handoff releases it only then, and the compaction leaves x's log alone.
        let dir = scratch("compacting");
        let path = dir.join("x.db");
        let store = Bucket::memory("").at("cluster");
        let registry = Rc::new(Registry::new(store.clone(), "a", "a", 10_000));
        let ownership = Ownership::new(store.clone(), registry.clone());
        let owners = Owners::new(store.clone());
        let rt = runtime();
        rt.block_on(registry.renew(0)).unwrap();
        let owned = match rt.block_on(ownership.acquire_or_route("ns", "x", 0)) {
            Ok(Claim::Owned(owned)) => owned,
            other => panic!("{other:?}"),
        };
        assert!(
            rt.block_on(owners.open_fresh(&ownership, "ns", "x", &owned))
                .unwrap()
        );
        rt.block_on(owners.take_over(&ownership, "ns", "x", &owned, &Root(path.clone())))
            .unwrap();
        let app = Connection::open(&path);
        app.execute("PRAGMA journal_mode=WAL; CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
        insert(&app, 0, 10);
        let wal = std::fs::read(dir.join("x.db-wal")).unwrap();
        owners.wal_write("ns", "x", "x.db", 0, &wal);
        rt.block_on(owners.commit(&ownership, "ns", "x")).unwrap();

        let x = format!("{}{}", store.prefix(), crate::object_prefix("ns", "x"));
        let x = Log::new(store.at(&x));
        let files = rt.block_on(x.files(1)).unwrap();
        let stopped = owners.while_locked("ns", "x", async {
            // The compaction starts, and waits for the lock.
            tokio::task::yield_now().await;
            owners.close("ns", "x");
            assert!(owners.busy("ns", "x"));
        });
        rt.block_on(async { futures::join!(stopped, owners.tier()) });
        assert!(!owners.busy("ns", "x"));
        assert_eq!(rt.block_on(x.files(1)).unwrap(), files);
    });
}

/// Where `Owners` places an object with only a root database.
struct Root(PathBuf);

impl Layout for Root {
    fn place(&self, lane: &str) -> crate::Result<PathBuf> {
        assert_eq!(lane, DB);
        Ok(self.0.clone())
    }

    fn clear(&self) -> crate::Result<()> {
        for suffix in ["", "-wal", "-shm"] {
            let _ = std::fs::remove_file(crate::log::with_suffix(&self.0, suffix));
        }
        Ok(())
    }
}

/// A facet's connection, its database captured as lane `lane`.
fn open_facet(path: &Path, lane: &str) -> Connection {
    CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().open_lane(path, lane));
    let db = Connection::capturing(path);
    db.execute(
        "PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, v TEXT)",
    );
    db
}

/// Removes a closed database's files, as deleting a facet does.
fn remove(path: &Path) {
    for suffix in ["", "-wal", "-shm"] {
        let _ = std::fs::remove_file(crate::log::with_suffix(path, suffix));
    }
}

/// Every lane the log restores, with its rows.
fn restored_lanes(store: &Bucket, name: &str) -> BTreeMap<String, (i64, i64, i64)> {
    let (_, lanes) = runtime()
        .block_on(restore(&log(store), 0, &scratch(name)))
        .unwrap();
    let open = |path: PathBuf| rows(&Connection::open(&path));
    lanes.into_iter().map(|(l, path)| (l, open(path))).collect()
}

fn lanes() -> Vec<Option<String>> {
    CAPTURE.with_borrow(|c| c.as_ref().unwrap().cursor().lanes)
}

fn want<const N: usize>(lanes: [(&str, &Connection); N]) -> BTreeMap<String, (i64, i64, i64)> {
    lanes
        .into_iter()
        .map(|(l, c)| (l.to_owned(), rows(c)))
        .collect()
}

#[test]
fn facets_restore_from_their_roots_log() {
    checked(|| {
        let dir = scratch("facets");
        let root = open_app(&dir.join("app.db"));
        let a = open_facet(&dir.join("app.1.db"), "a/db");
        let b = open_facet(&dir.join("app.2.db"), "a/b%2Fc/db");
        let store = Bucket::memory("");
        let mut shipped = 0;
        for i in 0..30 {
            insert(&root, i * 10, i * 10 + 10);
            insert(&a, i, i + 1);
            if i % 3 == 0 {
                insert(&b, i, i + 1);
            }
            if i % 7 == 6 {
                upload(&store, shipped, last());
                shipped = last();
            }
        }
        upload(&store, shipped, last());
        let want = want([(DB, &root), ("a/db", &a), ("a/b%2Fc/db", &b)]);
        assert_eq!(restored_lanes(&store, "facets-batches"), want);
        let store = Bucket::memory("");
        upload_snapshot(&store, last());
        assert_eq!(restored_lanes(&store, "facets-snapshot"), want);
    });
}

#[test]
fn a_batch_keeps_one_copy_of_each_lanes_rewritten_pages() {
    checked(|| {
        let dir = scratch("dedup-lanes");
        let root = open_app(&dir.join("app.db"));
        let a = open_facet(&dir.join("app.1.db"), "a/db");
        insert(&root, 0, 10);
        insert(&a, 0, 10);
        let mut ends = vec![last()];
        for i in 0..100 {
            // Each value's length is its version, which `rows` compares.
            let v = "v".repeat(i + 1);
            root.execute(&format!("UPDATE t SET v = '{v}' WHERE id = 1"));
            a.execute(&format!("UPDATE t SET v = '{v}' WHERE id = 2"));
            if i % 10 == 9 {
                ends.push(last());
            }
        }
        let raw = CAPTURE.with_borrow(|c| c.as_ref().unwrap().batch(ends[0], last()).unwrap());
        let merged = merged(ends[0], last());
        assert!(
            merged.len() * 20 < raw.len(),
            "{} of {}",
            merged.len(),
            raw.len()
        );
        let want = want([(DB, &root), ("a/db", &a)]);
        // A covered batch and one overlapping what came before, as tiering and
        // a commit racing leave them.
        let store = Bucket::memory("");
        upload(&store, 0, ends[3]);
        upload(&store, ends[1], ends[10]);
        upload(&store, ends[2], ends[6]);
        assert_eq!(restored_lanes(&store, "dedup-lanes-out"), want);
    });
}

#[test]
fn a_deleted_facet_stays_deleted_and_frees_its_lane() {
    checked(|| {
        let dir = scratch("delete-lanes");
        let root = open_app(&dir.join("app.db"));
        let (a_path, ab_path) = (dir.join("app.1.db"), dir.join("app.2.db"));
        let a = open_facet(&a_path, "a/db");
        let ab = open_facet(&ab_path, "a/b/db");
        let x = open_facet(&dir.join("app.3.db"), "x/db");
        for db in [&root, &a, &ab, &x] {
            insert(db, 0, 20);
        }
        let store = Bucket::memory("");
        upload(&store, 0, last());
        let before = last();
        drop((a, ab));
        remove(&a_path);
        remove(&ab_path);
        CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().delete_lanes("a/"));
        assert_eq!(lanes(), [Some(DB.into()), None, None, Some("x/db".into())]);
        insert(&root, 20, 30);
        upload(&store, before, last());
        let want_ = want([(DB, &root), ("x/db", &x)]);
        assert_eq!(restored_lanes(&store, "delete-lanes-out"), want_);
        // A snapshot with numbers freed between lanes.
        let snapshot = Bucket::memory("");
        upload_snapshot(&snapshot, last());
        assert_eq!(restored_lanes(&snapshot, "delete-lanes-snapshot"), want_);
        // The next facet takes the lowest free number.
        let c = open_facet(&dir.join("app.4.db"), "c/db");
        insert(&c, 0, 5);
        assert_eq!(lanes()[1].as_deref(), Some("c/db"));
        let store = Bucket::memory("");
        upload(&store, 0, last());
        let want = want([(DB, &root), ("x/db", &x), ("c/db", &c)]);
        assert_eq!(restored_lanes(&store, "delete-lanes-reused"), want);
    });
}

#[test]
fn a_reset_deletes_its_facets_descendants() {
    checked(|| {
        // A facet's deleteAll(): its database and its descendants' go, the
        // database starts over, and the root and other facets stay.
        let dir = scratch("reset-lanes");
        let root = open_app(&dir.join("app.db"));
        let (a_path, ab_path) = (dir.join("app.1.db"), dir.join("app.2.db"));
        let a = open_facet(&a_path, "a/db");
        let ab = open_facet(&ab_path, "a/b/db");
        let ax = open_facet(&dir.join("app.3.db"), "ax/db");
        for db in [&root, &a, &ab, &ax] {
            insert(db, 0, 20);
        }
        drop((a, ab));
        remove(&a_path);
        remove(&ab_path);
        CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().reset("app.1.db"));
        let a = open_facet(&a_path, "a/db");
        insert(&a, 100, 103);
        let store = Bucket::memory("");
        upload(&store, 0, last());
        let want = want([(DB, &root), ("a/db", &a), ("ax/db", &ax)]);
        assert_eq!(restored_lanes(&store, "reset-lanes-out"), want);
        // The root's reset takes every facet.
        drop((root, a, ax));
        remove(&dir.join("app.db"));
        CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().reset("app.db"));
        assert_eq!(lanes(), []);
    });
}

/// Copies a running facet's database to `to` with its WAL folded in, out of
/// the capture's sight, as cloning a facet does.
fn copy_folded(from: &Path, to: &Path) {
    for suffix in ["", "-wal"] {
        let with = |p| crate::log::with_suffix(p, suffix);
        std::fs::copy(with(from), with(to)).unwrap();
    }
    Connection::open(to).execute("PRAGMA wal_checkpoint(TRUNCATE)");
    for suffix in ["-wal", "-shm"] {
        let _ = std::fs::remove_file(crate::log::with_suffix(to, suffix));
    }
}

#[test]
fn a_clone_restores_as_its_source_was() {
    checked(|| {
        // Facet a cloned over facet c: c's subtree goes, a's is copied, and
        // both go on independently.
        let dir = scratch("clone-lanes");
        let root = open_app(&dir.join("app.db"));
        let p = |n: u32| dir.join(format!("app.{n}.db"));
        let a = open_facet(&p(1), "a/db");
        let ab = open_facet(&p(2), "a/b/db");
        let c = open_facet(&p(3), "c/db");
        let cd = open_facet(&p(4), "c/d/db");
        for db in [&root, &a, &ab, &c, &cd] {
            insert(db, 0, 20);
        }
        let store = Bucket::memory("");
        upload(&store, 0, last());
        let before = last();
        insert(&ab, 20, 25);
        drop((c, cd));
        remove(&p(3));
        remove(&p(4));
        copy_folded(&p(1), &p(5));
        copy_folded(&p(2), &p(6));
        CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().clone_lanes("a/", "c/"));
        insert(&a, 20, 30);
        let c = open_facet(&p(5), "c/db");
        let cb = open_facet(&p(6), "c/b/db");
        insert(&c, 100, 105);
        upload(&store, before, last());
        let want = want([
            (DB, &root),
            ("a/db", &a),
            ("a/b/db", &ab),
            ("c/db", &c),
            ("c/b/db", &cb),
        ]);
        assert_eq!(rows(&cb), rows(&ab));
        assert_eq!(restored_lanes(&store, "clone-lanes-out"), want);
        let snapshot = Bucket::memory("");
        upload_snapshot(&snapshot, last());
        assert_eq!(restored_lanes(&snapshot, "clone-lanes-snapshot"), want);
    });
}

#[test]
fn facet_paths_round_trip_through_lane_names() {
    use crate::log::facet_path;
    use crate::log::facet_prefix;
    for path in [vec![], vec!["a"], vec!["a/b", "%", "", "db", "\u{e9}"]] {
        let lane = facet_prefix(path.iter().copied()) + DB;
        assert_eq!(facet_path(&lane).unwrap(), path, "{lane}");
    }
    assert_eq!(facet_prefix(["a/b", "c"]) + DB, "a%2Fb/c/db");
    assert_eq!(facet_path("xdb"), None);
}
