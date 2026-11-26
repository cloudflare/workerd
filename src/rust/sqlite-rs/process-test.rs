// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::num::NonZeroUsize;

use sqlite_rs_testing::checked;

use super::*;
use crate::Database;
use crate::OpenMode;
use crate::Vfs;

const BLOB: &str = "SELECT length(randomblob(4000000));";

// Removes the limit when dropped, so that a failed assertion leaves none for the other tests.
struct RemoveLimit;

impl Drop for RemoveLimit {
    fn drop(&mut self) {
        let _ = set_max_memory_per_process_bytes(None);
    }
}

#[test]
fn max_memory_per_process_bytes_limits_every_database() {
    checked(|| {
        let vfs = Vfs::in_memory().unwrap();
        let before = Database::open(&vfs, "before.sqlite", OpenMode::Create).unwrap();
        assert_eq!(before.run(BLOB, &[]).unwrap().i64(0).unwrap(), 4_000_000);

        let limit = NonZeroUsize::new(3 << 20);
        let remove = RemoveLimit;
        assert_eq!(set_max_memory_per_process_bytes(limit).unwrap(), None);
        let after = Database::open(&vfs, "after.sqlite", OpenMode::Create).unwrap();
        for db in [&before, &after] {
            let error = db.run(BLOB, &[]).err().unwrap();
            assert!(error.description().contains("out of memory"), "{error:?}");
            assert_eq!(db.run("SELECT 1;", &[]).unwrap().i64(0).unwrap(), 1);
        }

        assert_eq!(set_max_memory_per_process_bytes(None).unwrap(), limit);
        drop(remove);
        for db in [&before, &after] {
            assert_eq!(db.run(BLOB, &[]).unwrap().i64(0).unwrap(), 4_000_000);
        }
    });
}
