// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::path::PathBuf;

use super::*;

#[test]
fn a_value_is_its_expiry_then_the_record() {
    let tmp = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let path = tmp.join("value");
    let read = |bytes: &[u8]| {
        std::fs::write(&path, bytes).unwrap();
        expiry(&mut File::open(&path).unwrap()).unwrap()
    };
    assert_eq!(
        read(b"1700000000000\nrecord"),
        Some((1_700_000_000_000, 14))
    );
    assert_eq!(read(b"soon\nrecord"), None);
    assert_eq!(read(b"1700000000000"), None);
}

#[test]
fn a_node_removes_only_directories_no_node_holds() {
    let tmp = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let root = tmp.join("claim");
    std::fs::create_dir_all(&root).unwrap();
    let (a, a_lock) = clustered::claim(&root, "a").unwrap();
    let (b, _b_lock) = clustered::claim(&root, "b").unwrap();
    assert!(a.exists());
    drop(a_lock);
    let (c, _c_lock) = clustered::claim(&root, "c").unwrap();
    assert!(!a.exists() && b.exists() && c.exists());
}
