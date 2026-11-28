// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use sqlite_rs_testing::checked;
use sqlite_rs_testing::temp_dir;

use super::*;
use crate::Database;
use crate::OpenMode;

fn write_row(vfs: &Vfs, path: &str) {
    let db = Database::open(vfs, path, OpenMode::CreateWithParents).unwrap();
    assert_eq!(
        db.run("PRAGMA journal_mode=WAL;", &[])
            .unwrap()
            .text(0)
            .unwrap(),
        "wal"
    );
    db.run(
        "CREATE TABLE t (v TEXT); INSERT INTO t VALUES ('persisted');",
        &[],
    )
    .unwrap();
}

fn read_row(vfs: &Vfs, path: &str) -> String {
    let db = Database::open(vfs, path, OpenMode::ReadWrite).unwrap();
    let rows = db.run("SELECT v FROM t;", &[]).unwrap();
    rows.text(0).unwrap()
}

#[test]
fn in_memory_persists_across_reopening() {
    checked(|| {
        let vfs = Vfs::in_memory().unwrap();
        write_row(&vfs, "dir/db.sqlite");
        assert_eq!(read_row(&vfs, "dir/db.sqlite"), "persisted");

        // Another in-memory VFS has a directory of its own.
        let other = Vfs::in_memory().unwrap();
        assert!(Database::open(&other, "dir/db.sqlite", OpenMode::ReadWrite).is_err());
    });
}

#[test]
fn on_disk_persists_across_reopening() {
    checked(|| {
        let dir = temp_dir("on-disk");
        let path = dir.to_str().unwrap();
        write_row(&Vfs::on_disk(path).unwrap(), "sub/db.sqlite");
        assert!(dir.join("sub/db.sqlite").is_file());
        assert_eq!(
            read_row(&Vfs::on_disk(path).unwrap(), "sub/db.sqlite"),
            "persisted"
        );
    });
}

#[test]
fn database_keeps_its_vfs() {
    checked(|| {
        let vfs = Vfs::in_memory().unwrap();
        let db = Database::open(&vfs, "db.sqlite", OpenMode::Create).unwrap();
        drop(vfs);
        db.run("CREATE TABLE t (v); INSERT INTO t VALUES (1);", &[])
            .unwrap();
        let vfs = db.vfs().clone();
        drop(db);
        assert!(Database::open(&vfs, "db.sqlite", OpenMode::ReadOnly).is_ok());
    });
}

#[test]
fn vfs_exception_is_the_error() {
    checked(|| {
        let vfs = Vfs::in_memory().unwrap();
        // Creates the directory `dir`, which SQLite then tries to open as a database: the
        // in-memory directory throws "not a file" in the VFS's xOpen.
        Database::open(&vfs, "dir/db.sqlite", OpenMode::CreateWithParents).unwrap();
        for mode in [OpenMode::ReadOnly, OpenMode::ReadWrite] {
            let error = Database::open(&vfs, "dir", mode).err().unwrap();
            assert!(error.description().contains("not a file"), "{error:?}");
            assert!(
                !error.description().contains("SQLITE_CANTOPEN"),
                "{error:?}"
            );
        }
    });
}

#[test]
fn open_errors() {
    checked(|| {
        let vfs = Vfs::in_memory().unwrap();
        // A missing file is SQLite's error: the VFS reports it without an exception.
        let error = Database::open(&vfs, "missing.sqlite", OpenMode::ReadOnly)
            .err()
            .unwrap();
        assert!(error.description().contains("SQLITE_CANTOPEN"), "{error:?}");

        assert!(Database::open(&vfs, "../db.sqlite", OpenMode::Create).is_err());

        let dir = temp_dir("open-errors");
        assert!(Vfs::on_disk(dir.join("missing").to_str().unwrap()).is_err());
    });
}

#[test]
fn vfs_open_exception_during_a_step_is_sqlite_cantopen() {
    checked(|| {
        let vfs = Vfs::in_memory().unwrap();
        let db = Database::open(&vfs, "db.sqlite", OpenMode::Create).unwrap();
        db.run("CREATE TABLE t (v); INSERT INTO t VALUES (1);", &[])
            .unwrap();
        // A directory where SQLite keeps the database's rollback journal. The next statement
        // looks for a hot journal and opens it, and the in-memory directory throws "not a file"
        // in the VFS's xOpen. xOpen reports that to SQLite as SQLITE_CANTOPEN, and
        // `SqliteDatabase` rethrows a VFS exception caught during a step only for SQLITE_IOERR,
        // so the error is SQLite's, without the exception, and nothing is logged. This pins that
        // behaviour; https://github.com/cloudflare/workerd/issues/8942 tracks returning the
        // exception.
        Database::open(
            &vfs,
            "db.sqlite-journal/x.sqlite",
            OpenMode::CreateWithParents,
        )
        .unwrap();
        for sql in ["SELECT v FROM t;", "INSERT INTO t VALUES (2);"] {
            let error = db.run(sql, &[]).err().unwrap();
            assert!(error.description().contains("SQLITE_CANTOPEN"), "{error:?}");
            assert!(!error.description().contains("not a file"), "{error:?}");
        }
    });
}

// SQLITE_IOCAP_IMMUTABLE: the files never change, so SQLite opens them read-only.
const IMMUTABLE: i32 = 0x2000;

#[test]
fn device_characteristics_reach_sqlite_in_memory() {
    checked(|| {
        let immutable =
            Vfs::in_memory_with(VfsOptions::new().device_characteristics(IMMUTABLE)).unwrap();
        let db = Database::open(&immutable, "db.sqlite", OpenMode::Create).unwrap();
        let error = db.run("CREATE TABLE t (v TEXT);", &[]).err().unwrap();
        assert!(error.description().contains("readonly"), "{error:?}");

        let db = Database::open(&Vfs::in_memory().unwrap(), "db.sqlite", OpenMode::Create).unwrap();
        db.run("CREATE TABLE t (v TEXT);", &[]).unwrap();
    });
}

// On Windows, `SqliteDatabase` opens a database on disk through SQLite's own VFS, which reports
// its own device characteristics.
#[cfg(not(windows))]
#[test]
fn device_characteristics_reach_sqlite_on_disk() {
    checked(|| {
        let dir = temp_dir("immutable");
        let path = dir.to_str().unwrap();
        write_row(&Vfs::on_disk(path).unwrap(), "db.sqlite");

        let immutable =
            Vfs::on_disk_with(path, VfsOptions::new().device_characteristics(IMMUTABLE)).unwrap();
        assert_eq!(read_row(&immutable, "db.sqlite"), "persisted");
        let db = Database::open(&immutable, "db.sqlite", OpenMode::ReadWrite).unwrap();
        let error = db.run("INSERT INTO t VALUES ('more');", &[]).err().unwrap();
        assert!(error.description().contains("readonly"), "{error:?}");
    });
}

#[test]
fn vfs_without_sync_persists_across_reopening() {
    checked(|| {
        let dir = temp_dir("without-sync");
        let path = dir.to_str().unwrap();
        let options = VfsOptions::new().without_sync();
        write_row(&Vfs::on_disk_with(path, options).unwrap(), "db.sqlite");
        assert_eq!(
            read_row(&Vfs::on_disk_with(path, options).unwrap(), "db.sqlite"),
            "persisted"
        );

        let vfs = Vfs::in_memory_with(options).unwrap();
        write_row(&vfs, "db.sqlite");
        assert_eq!(read_row(&vfs, "db.sqlite"), "persisted");
    });
}
