// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(clippy::unwrap_used)]

use cluster::Bucket;
use futures::executor::block_on;

use super::*;

/// The least size of a part but the last, as S3's.
const MIN_PART: u64 = 5 << 20;

fn dir() -> PathBuf {
    let base = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    base.join(format!("bucket-{}", random()))
}

fn local(dir: &Path, min_part: u64) -> Bucket {
    Bucket::new(Local::open(dir, min_part).unwrap(), "p")
}

#[test]
fn keys_a_file_tree_cannot_hold_and_a_reopened_store() {
    block_on(async {
        let dir = dir();
        let bucket = local(&dir, MIN_PART);
        for key in ["a", "a/b", "/a//b", "../x", ""] {
            bucket.put(key, key.as_bytes().to_vec(), &[]).await.unwrap();
        }
        drop(bucket);
        let bucket = local(&dir, MIN_PART);
        for key in ["a", "a/b", "/a//b", "../x", ""] {
            let (body, _) = bucket.get(key).await.unwrap().unwrap();
            assert_eq!(body, key.as_bytes());
        }
        bucket.delete("a").await.unwrap();
        bucket.delete("a").await.unwrap();
        let keys: Vec<_> = bucket.list("").await.unwrap();
        let keys: Vec<_> = keys.into_iter().map(|e| e.key).collect();
        assert_eq!(keys, ["", "../x", "/a//b", "a/b"]);
        // Only live blobs remain: one per object, none in `tmp`.
        let count = |d: &str| std::fs::read_dir(dir.join(d)).unwrap().count();
        assert_eq!((count("blobs"), count("tmp")), (4, 0));
    });
}

#[test]
fn a_smaller_least_part_and_parts_dropped() {
    block_on(async {
        let dir = dir();
        let bucket = local(&dir, 2);
        let upload = bucket.begin("k", &Headers::default(), &[]).await.unwrap();
        let e1 = bucket.part("k", &upload, 1, "a".into()).await.unwrap();
        let small = [(1, e1), (2, String::new())];
        let small = bucket.complete("k", &upload, &small).await;
        assert!(matches!(small, Err(Error::Refused { .. })));
        // A part sent again replaces the first.
        let e1 = bucket.part("k", &upload, 1, "ab".into()).await.unwrap();
        let e2 = bucket.part("k", &upload, 2, "c".into()).await.unwrap();
        bucket.part("k", &upload, 3, "unused".into()).await.unwrap();
        let parts = [(1, e1), (2, e2)];
        bucket.complete("k", &upload, &parts).await.unwrap();
        let (body, _) = bucket.get("k").await.unwrap().unwrap();
        assert_eq!(body, b"abc");
        let count = |d: &str| std::fs::read_dir(dir.join(d)).unwrap().count();
        assert_eq!((count("blobs"), count("tmp")), (1, 0));
    });
}
