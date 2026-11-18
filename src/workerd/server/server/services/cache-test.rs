// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::path::PathBuf;

use super::*;

#[test]
fn a_value_is_its_expiry_and_names_then_the_record() {
    let tmp = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let path = tmp.join("value");
    let write = |bytes: &[u8]| std::fs::write(&path, bytes).unwrap();
    let read = |bytes: &[u8]| {
        write(bytes);
        expiry(&mut File::open(&path).unwrap()).unwrap()
    };
    assert_eq!(
        read(b"1700000000000 0 2\n{}record"),
        Some((1_700_000_000_000, 0, 2, 18))
    );
    assert_eq!(read(b"1700000000000 0\nrecord"), None);
    assert_eq!(read(b"soon 0 0\nrecord"), None);
    assert_eq!(read(b"1700000000000 0 0"), None);
    write(b"1 0 0\nrecord");
    assert!(meta(&mut File::open(&path).unwrap()).unwrap().is_none());
    let names = |ns: &str, path: &str, tags: &str| {
        let headers = [("X-Purge-Namespace", ns), ("X-Purge-Path", path)];
        let headers = [headers[0], headers[1], ("X-Purge-Tags", tags)];
        Meta::from(|n| headers.iter().find(|h| h.0 == n).map(|h| h.1.to_owned()))
    };
    let bytes = names("a", "/x/y?z", "t1, T2").unwrap().bytes();
    write(&[format!("1 {} 0\n", bytes.len()).as_bytes(), &bytes, b"record"].concat());
    let meta = meta(&mut File::open(&path).unwrap()).unwrap().unwrap();
    let purge = |q: &str| Purge::parse(format!("http://store/?{q}").as_bytes()).unwrap();
    for q in ["ns=a&tag=t2", "ns=a&prefix=/x/", "ns=a&everything", "ns=a&tag=no&tag=t1"] {
        assert!(purge(q).matches(&meta), "{q}");
    }
    let untagged = names("a", "/x", "").unwrap();
    assert!(!purge("ns=a&tag=").matches(&untagged));
    for q in ["ns=b&everything", "everything", "ns=a&tag=t", "ns=a&prefix=/y"] {
        assert!(!purge(q).matches(&meta), "{q}");
    }
}

#[test]
fn a_fill_stores_unless_a_purge_since_matches() {
    let purges = Rc::new(Purges::default());
    let meta = |tag: &str| Meta::from(|n| (n == "X-Purge-Tags").then(|| tag.to_owned())).unwrap();
    let purge = Purge::parse(b"http://store/?tag=a").unwrap();
    let before = purges.ticket(true);
    assert!(purges.store(&purges.ticket(false), &meta("a")).is_none());
    let storing = purges.store(&before, &meta("a")).unwrap();
    let waker = futures::task::noop_waker();
    let mut cx = std::task::Context::from_waker(&waker);
    let mut fence = std::pin::pin!(purges.fence(&purge));
    assert!(fence.as_mut().poll(&mut cx).is_pending(), "a fence waits for fills storing");
    drop(storing);
    assert!(fence.as_mut().poll(&mut cx).is_ready());
    assert!(purges.store(&before, &meta("a")).is_none());
    assert!(purges.store(&before, &meta("b")).is_some());
    assert!(purges.store(&purges.ticket(true), &meta("a")).is_some());
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
