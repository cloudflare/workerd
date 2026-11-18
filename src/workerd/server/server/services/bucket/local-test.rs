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
        // The object is its two parts' blobs, uncopied; the unused part's is
        // gone. A range reads across them.
        let count = |d: &str| std::fs::read_dir(dir.join(d)).unwrap().count();
        assert_eq!((count("blobs"), count("tmp")), (2, 0));
        let range = Some(Range::From {
            offset: 1,
            length: Some(2),
        });
        let (_, body) = bucket.read("k", range).await.unwrap().unwrap();
        let body: Vec<_> = body.collect().await;
        let body: Vec<u8> = body
            .into_iter()
            .map(Result::unwrap)
            .collect::<Vec<_>>()
            .concat();
        assert_eq!(body, b"bc");
        // A rewrite drops them both.
        bucket.put("k", b"x".to_vec(), &[]).await.unwrap();
        assert_eq!(count("blobs"), 1);
    });
}

#[test]
fn an_upload_id_begin_could_not_have_made_is_no_upload() {
    block_on(async {
        let dir = dir();
        let bucket = local(&dir, MIN_PART);
        let upload = bucket.begin("k", &Headers::default(), &[]).await.unwrap();
        // A copy of the upload record outside `uploads/`, which a path
        // built from the ID would reach.
        let record = std::fs::read(dir.join("uploads").join(&upload)).unwrap();
        std::fs::write(dir.join("outside"), record).unwrap();
        let absolute = dir.join("outside").display().to_string();
        for id in ["../outside", &absolute, &upload.to_uppercase()] {
            let refused = bucket.parts("k", id).await;
            assert!(
                matches!(&refused, Err(Error::Refused { status: 404, code, .. }) if code == "NoSuchUpload"),
                "{id}: {refused:?}"
            );
        }
        assert!(bucket.parts("k", &upload).await.unwrap().is_empty());
    });
}

#[test]
fn stores_of_one_directory_share_it() {
    block_on(async {
        let dir = dir();
        let (a, b) = (local(&dir, MIN_PART), local(&dir, MIN_PART));
        a.put("k", b"one".to_vec(), &[]).await.unwrap();
        let (body, seen) = b.get("k").await.unwrap().unwrap();
        assert_eq!(body, b"one");
        // A read under way keeps the bytes it began with.
        let (_, reading) = a.read("k", None).await.unwrap().unwrap();
        b.put("k", b"two".to_vec(), &[]).await.unwrap();
        let read: Vec<_> = reading.collect::<Vec<_>>().await;
        assert_eq!(
            read.into_iter()
                .map(Result::unwrap)
                .collect::<Vec<_>>()
                .concat(),
            b"one"
        );
        // Each sees the other's writes, guards included.
        assert_eq!(a.get("k").await.unwrap().unwrap().0, b"two");
        let stale = a.put_if("k", b"three".to_vec(), Guard::Match(seen)).await;
        assert_eq!(stale.unwrap(), None);
        assert_eq!(b.put_if("k", vec![], Guard::Absent).await.unwrap(), None);
        b.put("l", vec![], &[]).await.unwrap();
        let keys: Vec<_> = a
            .list("")
            .await
            .unwrap()
            .into_iter()
            .map(|e| e.key)
            .collect();
        assert_eq!(keys, ["k", "l"]);
        b.delete("k").await.unwrap();
        assert!(a.get("k").await.unwrap().is_none());
    });
}

#[test]
fn an_upload_completes_once_across_stores() {
    block_on(async {
        let dir = dir();
        let (a, b) = (local(&dir, 1), local(&dir, 1));
        let upload = a.begin("k", &Headers::default(), &[]).await.unwrap();
        let parts = [(1, a.part("k", &upload, 1, "ab".into()).await.unwrap())];
        b.complete("k", &upload, &parts).await.unwrap();
        let again = a.complete("k", &upload, &parts).await;
        assert!(matches!(again, Err(Error::Refused { .. })), "{again:?}");
        assert_eq!(a.get("k").await.unwrap().unwrap().0, b"ab");
        // A part for an upload that is gone keeps no blob.
        let late = b.part("k", &upload, 2, "c".into()).await;
        assert!(matches!(late, Err(Error::Refused { .. })), "{late:?}");
        let blobs = std::fs::read_dir(dir.join("blobs")).unwrap().count();
        assert_eq!(blobs, 1);
    });
}
