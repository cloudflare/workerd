// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The capture against real `SQLite`: a test VFS wraps the default one and
//! reports WAL writes to a `Capture`, as workerd's VFS does, and whatever
//! the capture cut must restore to what the application sees.

use std::cell::RefCell;
use std::ffi::CString;
use std::ffi::c_char;
use std::ffi::c_int;
use std::ffi::c_void;
use std::path::Path;
use std::path::PathBuf;
use std::rc::Rc;
use std::sync::OnceLock;

use crate::Bucket;
use crate::Error;
use crate::Sequence;
use crate::capture::Capture;
use crate::log::File as LogFile;
use crate::log::Log;
use crate::log::restore;
use crate::owner::Owners;
use crate::ownership::Claim;
use crate::ownership::Ownership;
use crate::registry::Registry;

/// `sqlite3_vfs`, with the members the wrapper replaces typed.
#[repr(C)]
#[derive(Clone, Copy)]
struct Vfs {
    version: c_int,
    file_size: c_int,
    max_path: c_int,
    next: *mut Self,
    name: *const c_char,
    app_data: *mut c_void,
    open: unsafe extern "C" fn(*mut Self, *const c_char, *mut File, c_int, *mut c_int) -> c_int,
    rest: [*const c_void; 15],
}

/// `sqlite3_io_methods`, likewise.
#[repr(C)]
#[derive(Clone, Copy)]
struct Methods {
    version: c_int,
    close: *const c_void,
    read: *const c_void,
    write: unsafe extern "C" fn(*mut File, *const c_void, c_int, i64) -> c_int,
    rest: [*const c_void; 15],
}

/// A file's own method table, followed by the method it replaces and
/// whether the file is a WAL. It sits past the wrapped file's state.
#[repr(C)]
struct Patched {
    methods: Methods,
    wrapped_write: unsafe extern "C" fn(*mut File, *const c_void, c_int, i64) -> c_int,
    is_wal: bool,
}

#[repr(C)]
struct File {
    methods: *const Methods,
}

unsafe extern "C" {
    fn sqlite3_vfs_find(name: *const c_char) -> *mut Vfs;
    fn sqlite3_vfs_register(vfs: *mut Vfs, default: c_int) -> c_int;
    fn sqlite3_open_v2(
        path: *const c_char,
        db: *mut *mut c_void,
        flags: c_int,
        vfs: *const c_char,
    ) -> c_int;
    fn sqlite3_exec(
        db: *mut c_void,
        sql: *const c_char,
        cb: *const c_void,
        arg: *mut c_void,
        err: *mut *mut c_char,
    ) -> c_int;
    fn sqlite3_prepare_v2(
        db: *mut c_void,
        sql: *const c_char,
        n: c_int,
        stmt: *mut *mut c_void,
        tail: *mut *const c_char,
    ) -> c_int;
    fn sqlite3_step(stmt: *mut c_void) -> c_int;
    fn sqlite3_column_int64(stmt: *mut c_void, col: c_int) -> i64;
    fn sqlite3_finalize(stmt: *mut c_void) -> c_int;
    fn sqlite3_close(db: *mut c_void) -> c_int;
}

/// Just enough of `SQLite` for the tests: run SQL, read one row of integers.
struct Connection(*mut c_void);

impl Connection {
    fn open(path: &str) -> Self {
        let path = CString::new(path).unwrap();
        let mut db = std::ptr::null_mut();
        // OPEN_READWRITE | OPEN_CREATE | OPEN_URI.
        // SAFETY: the arguments are valid for the call.
        let rc = unsafe { sqlite3_open_v2(path.as_ptr(), &raw mut db, 0x46, std::ptr::null()) };
        assert_eq!(rc, 0);
        Self(db)
    }

    fn execute(&self, sql: &str) {
        let sql = CString::new(sql).unwrap();
        // SAFETY: `self.0` is an open connection.
        let rc = unsafe {
            sqlite3_exec(
                self.0,
                sql.as_ptr(),
                std::ptr::null(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            )
        };
        assert_eq!(rc, 0, "{sql:?}");
    }

    fn row(&self, sql: &str, columns: c_int) -> Vec<i64> {
        let sql = CString::new(sql).unwrap();
        let mut stmt = std::ptr::null_mut();
        // SAFETY: `self.0` is an open connection and `stmt` is finalized.
        unsafe {
            assert_eq!(
                sqlite3_prepare_v2(
                    self.0,
                    sql.as_ptr(),
                    -1,
                    &raw mut stmt,
                    std::ptr::null_mut()
                ),
                0
            );
            assert_eq!(sqlite3_step(stmt), 100);
            let row = (0..columns)
                .map(|c| sqlite3_column_int64(stmt, c))
                .collect();
            sqlite3_finalize(stmt);
            row
        }
    }
}

impl Drop for Connection {
    fn drop(&mut self) {
        // SAFETY: `self.0` is an open connection, closed once.
        unsafe { sqlite3_close(self.0) };
    }
}

const SQLITE_OPEN_WAL: c_int = 0x0008_0000;

thread_local! {
    static CAPTURE: RefCell<Option<Capture>> = const { RefCell::new(None) };
}

/// The wrapped VFS, and where a file's `Patched` sits.
struct Wrapped {
    vfs: usize,
    patched_at: usize,
}

fn wrapped() -> &'static Wrapped {
    static WRAPPED: OnceLock<Wrapped> = OnceLock::new();
    // SAFETY: the default VFS lives for the process, and the copy
    // registered here is leaked, as `SQLite` requires.
    WRAPPED.get_or_init(|| unsafe {
        let native = sqlite3_vfs_find(std::ptr::null());
        let patched_at = usize::try_from((*native).file_size)
            .unwrap()
            .next_multiple_of(8);
        let mut vfs = *native;
        vfs.name = c"capture-test".as_ptr();
        vfs.file_size = c_int::try_from(patched_at + size_of::<Patched>()).unwrap();
        vfs.open = open;
        assert_eq!(sqlite3_vfs_register(Box::leak(Box::new(vfs)), 0), 0);
        Wrapped {
            vfs: native as usize,
            patched_at,
        }
    })
}

unsafe extern "C" fn open(
    _: *mut Vfs,
    name: *const c_char,
    file: *mut File,
    flags: c_int,
    out: *mut c_int,
) -> c_int {
    // SAFETY: `SQLite` passes a file of the wrapper's size, so the
    // patched table lives as long as the file.
    unsafe {
        let native = wrapped().vfs as *mut Vfs;
        let rc = ((*native).open)(native, name, file, flags, out);
        if rc == 0 && !(*file).methods.is_null() {
            let methods = *(*file).methods;
            let patched: *mut Patched = file.cast::<u8>().add(wrapped().patched_at).cast();
            patched.write(Patched {
                methods: Methods { write, ..methods },
                wrapped_write: methods.write,
                is_wal: flags & SQLITE_OPEN_WAL != 0,
            });
            (*file).methods = patched.cast_const().cast();
        }
        rc
    }
}

unsafe extern "C" fn write(file: *mut File, data: *const c_void, n: c_int, offset: i64) -> c_int {
    // SAFETY: `data` holds `n` bytes, and the file was opened by `open`.
    unsafe {
        let patched = &*(*file).methods.cast::<Patched>();
        let rc = (patched.wrapped_write)(file, data, n, offset);
        if rc == 0 && patched.is_wal {
            let data = std::slice::from_raw_parts(data.cast::<u8>(), usize::try_from(n).unwrap());
            let offset = u64::try_from(offset).unwrap();
            CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().write(offset, data));
        }
        rc
    }
}

fn scratch(name: &str) -> PathBuf {
    let root = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let dir = root.join(format!("capture-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// The application's connection, through the capturing VFS, with the
/// capture already watching.
fn open_app(path: &Path) -> Connection {
    wrapped();
    CAPTURE.set(Some(Capture::open(path).unwrap()));
    let app = Connection::open(&format!("file:{}?vfs=capture-test", path.display()));
    app.execute("PRAGMA journal_mode=WAL; CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
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

/// The captured frames `from..to` as one batch in `dir`.
fn batch(dir: &Path, from: Sequence, to: Sequence) -> (LogFile, PathBuf) {
    let bytes = CAPTURE.with_borrow(|c| c.as_ref().unwrap().batch(from, to).unwrap());
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

fn restored_rows(store: &Bucket, out: &Path) -> (i64, i64, i64) {
    runtime().block_on(restore(&log(store), 0, out)).unwrap();
    rows(&Connection::open(out.to_str().unwrap()))
}

#[test]
fn restore_reproduces_the_application_rows() {
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
}

#[test]
fn restore_reads_the_newest_snapshot_and_what_extends_it() {
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
}

#[test]
fn a_snapshot_inside_a_wal_carries_it_across_checkpoints() {
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
}

#[test]
fn a_snapshot_compacts_from_the_last_one_inside_a_wal() {
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
}

#[test]
fn a_corrupt_file_fails_to_restore() {
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
    let restored = runtime().block_on(restore(&log(&store), 0, &dir.join("restored.db")));
    assert!(restored.is_err(), "{restored:?}");
}

#[test]
fn a_rolled_back_transaction_leaves_nothing() {
    let dir = scratch("rollback");
    let app = open_app(&dir.join("app.db"));
    let store = Bucket::memory("");
    insert(&app, 0, 10);
    let wal = || std::fs::metadata(dir.join("app.db-wal")).unwrap().len();
    let before = wal();
    // A small cache spills the doomed transaction's frames into the WAL.
    app.execute("PRAGMA cache_size=2; BEGIN;");
    app.execute(
        "WITH RECURSIVE s(x) AS (SELECT 1000 UNION ALL SELECT x + 1 FROM s WHERE x < 3000) \
         INSERT INTO t SELECT x, hex(randomblob(100)) FROM s",
    );
    assert!(wal() > before + 100 * 4096, "no frames spilled");
    app.execute("ROLLBACK;");
    insert(&app, 10, 20);
    upload(&store, 0, last());
    assert_eq!(rows(&app).0, 20);
    assert_eq!(restored_rows(&store, &dir.join("restored.db")), rows(&app));
}

#[test]
fn checkpoints_and_a_restart_keep_the_log_whole() {
    let dir = scratch("checkpoint");
    let app = open_app(&dir.join("app.db"));
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
    app.execute("DELETE FROM t WHERE id > 20; VACUUM;");
    insert(&app, 5000, 5010);
    let store = Bucket::memory("");
    upload(&store, 0, last());
    assert_eq!(rows(&app).0, 31);
    assert_eq!(restored_rows(&store, &dir.join("two.db")), rows(&app));
}

#[test]
fn a_restart_after_the_database_closed_reads_the_file() {
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
    let app = Connection::open(&format!(
        "file:{}?vfs=capture-test",
        dir.join("app.db").display()
    ));
    assert_eq!(rows(&app), want);
    assert_eq!(std::fs::metadata(dir.join("app.db-wal")).unwrap().len(), 0);
    CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().restart().unwrap());
    let store = Bucket::memory("");
    upload(&store, 0, last());
    assert_eq!(restored_rows(&store, &dir.join("restored.db")), want);
}

#[test]
fn a_batch_the_log_already_covers_is_not_applied() {
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
}

#[test]
fn a_deleted_database_restores_empty_then_as_recreated() {
    // A facet's delete() or a deleteAll() removes the file; the new
    // database's first page then reaches the file, not the WAL.
    let dir = scratch("reset");
    let path = dir.join("app.db");
    let app = open_app(&path);
    insert(&app, 0, 50);
    drop(app);
    std::fs::remove_file(&path).unwrap();
    CAPTURE.with_borrow_mut(|c| c.as_mut().unwrap().reset());
    let deleted = last();
    let store = Bucket::memory("");
    upload(&store, 0, deleted);
    let out = dir.join("empty.db");
    runtime().block_on(restore(&log(&store), 0, &out)).unwrap();
    assert_eq!(std::fs::metadata(&out).unwrap().len(), 0);

    let app = Connection::open(&format!("file:{}?vfs=capture-test", path.display()));
    app.execute("PRAGMA journal_mode=WAL; CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
    insert(&app, 100, 110);
    upload(&store, deleted, last());
    assert_eq!(rows(&app).0, 10);
    assert_eq!(restored_rows(&store, &dir.join("split.db")), rows(&app));
    // As one batch.
    let store = Bucket::memory("");
    upload(&store, 0, last());
    assert_eq!(restored_rows(&store, &dir.join("merged.db")), rows(&app));
}

#[test]
fn a_superseded_owners_commit_is_not_acknowledged() {
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
    rt.block_on(a_owners.take_over(&a_own, "ns", "x", &owned, &path))
        .unwrap();

    let app = Connection::open(path.to_str().unwrap());
    app.execute("PRAGMA journal_mode=WAL; CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
    insert(&app, 0, 10);
    let wal = std::fs::read(dir.join("x.db-wal")).unwrap();
    a_owners.wal_write("ns", "x", 0, &wal[..32]);
    a_owners.wal_write("ns", "x", 32, &wal[32..]);

    // a stops renewing; b takes x over at epoch 2.
    rt.block_on(b.renew(9_000)).unwrap();
    let owned = claim(&b_own, 11_000);
    assert_eq!(owned.epoch, 2);
    rt.block_on(b_owners.take_over(&b_own, "ns", "x", &owned, &dir.join("b.db")))
        .unwrap();

    let commit = rt.block_on(a_owners.commit(&a_own, "ns", "x"));
    assert!(
        matches!(&commit, Err(Error::Contended(m)) if m.contains("superseded")),
        "{commit:?}"
    );
}
