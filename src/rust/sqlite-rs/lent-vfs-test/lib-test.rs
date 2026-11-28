// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use sqlite_rs::Database;
use sqlite_rs::OpenMode;
use sqlite_rs::Vfs;
use sqlite_rs_testing::checked;

use super::ffi;

#[test]
fn lent_vfs_keeps_its_owner_alive() {
    checked(|| {
        let vfs = Vfs::from_kj(ffi::vfs_lent_by_new_owner().unwrap());
        assert_eq!(ffi::vfs_owners_alive(), 1, "the lent VFS holds its owner");

        let db = Database::open(&vfs, "db.sqlite", OpenMode::Create).unwrap();
        db.run("CREATE TABLE t (v); INSERT INTO t VALUES (7);", &[])
            .unwrap();
        let mut select = db.prepare("SELECT v FROM t;").unwrap();
        drop(vfs);
        assert_eq!(ffi::vfs_owners_alive(), 1, "the database holds the VFS");
        drop(db);
        assert_eq!(
            ffi::vfs_owners_alive(),
            1,
            "the statement holds the database"
        );

        assert_eq!(select.run(&[]).unwrap().i64(0).unwrap(), 7);
        let reopened =
            Database::open(select.database().vfs(), "db.sqlite", OpenMode::ReadOnly).unwrap();
        assert_eq!(
            reopened
                .run("SELECT v FROM t;", &[])
                .unwrap()
                .i64(0)
                .unwrap(),
            7
        );
        drop(reopened);
        drop(select);
        assert_eq!(
            ffi::vfs_owners_alive(),
            0,
            "released after the last statement"
        );
    });
}
