// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use workerd_capnp::worker;
use workerd_capnp::worker::durable_object_storage;

use super::*;

fn words(message: &message::Builder<message::HeapAllocator>) -> Vec<u64> {
    capnp::serialize::write_message_to_words(message)
        .chunks_exact(8)
        .map(|c| u64::from_ne_bytes(c.try_into().unwrap()))
        .collect()
}

/// Workers `name` and "b", each binding a `kvNamespaceId` if `kv`, and
/// `builtinStorage` if `storage`.
fn config(name: &str, kv: bool, storage: Option<&str>) -> Vec<u64> {
    let mut message = message::Builder::new_default();
    let mut root = message.init_root::<config::Builder>();
    if let Some(storage) = storage {
        root.set_builtin_storage(storage);
    }
    let mut services = root.init_services(2);
    for (i, n) in (0u32..).zip([name, "b"]) {
        let mut s = services.reborrow().get(i);
        s.set_name(n);
        let w = s.init_worker();
        if kv {
            let mut b = w.init_bindings(1).get(0);
            b.set_name("KV");
            b.set_kv_namespace_id("ns");
        }
    }
    words(&message)
}

#[test]
fn no_kv_namespace_id_no_builtin() {
    assert!(rewrite(&config("a", false, None)).unwrap().is_none());
}

/// The built-in's Worker in `rewrite(config)`.
fn builtin(config: &[u64], f: impl FnOnce(worker::Reader<'_>)) {
    let out = rewrite(config).unwrap().unwrap();
    let bytes: Vec<u8> = out.iter().flat_map(|w| w.to_ne_bytes()).collect();
    let message = capnp::serialize::read_message(&mut &bytes[..], ReaderOptions::new()).unwrap();
    let services = message
        .get_root::<config::Reader>()
        .unwrap()
        .get_services()
        .unwrap();
    let kv = services.get(services.len() - 1);
    assert_eq!(kv.get_name().unwrap(), NAME);
    let service::Which::Worker(w) = kv.which().unwrap() else {
        panic!("not a worker");
    };
    f(w.unwrap());
}

#[test]
fn the_builtin_uses_builtin_storage() {
    builtin(&config("a", true, Some("store")), |w| {
        let storage = w.get_durable_object_storage();
        let durable_object_storage::Which::LocalDisk(d) = storage.which().unwrap() else {
            panic!("not localDisk");
        };
        assert_eq!(d.unwrap(), "store");
    });
}

/// The built-in's `CLUSTERED` binding, for a config with a cluster if
/// `cluster`.
fn clustered(cluster: bool) -> String {
    let mut message = message::Builder::new_default();
    let mut root = message.init_root::<config::Builder>();
    root.set_builtin_storage("store");
    if cluster {
        root.reborrow().init_cluster();
    }
    let mut s = root.init_services(1).get(0);
    s.set_name("a");
    let mut b = s.init_worker().init_bindings(1).get(0);
    b.set_name("KV");
    b.set_kv_namespace_id("ns");
    let mut out = String::new();
    builtin(&words(&message), |w| {
        for b in w.get_bindings().unwrap() {
            if b.get_name().unwrap() == "CLUSTERED" {
                let worker::binding::Which::Json(j) = b.which().unwrap() else {
                    panic!("not json");
                };
                out = j.unwrap().to_string().unwrap();
            }
        }
    });
    out
}

#[test]
fn the_builtin_knows_whether_it_is_clustered() {
    assert_eq!(clustered(false), "false");
    assert_eq!(clustered(true), "true");
}

#[test]
fn the_builtin_prefix_is_reserved() {
    for name in [NAME, BLOBS, "workerd:x"] {
        let e = rewrite(&config(name, false, None)).unwrap_err();
        assert!(e.to_string().contains("reserved"), "{name}: {e}");
    }
    assert!(rewrite(&config("workerd", false, None)).unwrap().is_none());
}

#[test]
fn a_kv_namespace_id_needs_builtin_storage() {
    let e = rewrite(&config("a", true, None)).unwrap_err();
    assert!(e.to_string().contains("needs Config.builtinStorage"), "{e}");
}

#[test]
fn blob_paths() {
    assert!(is_blob("0a/b/ff"));
    assert!(!is_blob("0a/b/"));
    assert!(!is_blob("../b/ff"));
    assert!(!is_blob("0a/b/ff/../x"));
    assert!(!is_blob("0A/b/ff"));
}

#[test]
fn the_blob_cache_holds_its_bound() {
    let dir = PathBuf::from(std::env::var("TEST_TMPDIR").unwrap()).join("blob-cache");
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(dir.join("stale"), b"from before").unwrap();
    let cache = BlobCache::new(dir.clone(), 10).unwrap();
    let on_disk = || -> u64 {
        std::fs::read_dir(&dir)
            .unwrap()
            .map(|e| e.unwrap().metadata().unwrap().len())
            .sum()
    };
    assert_eq!(on_disk(), 0);
    for id in ["a", "b", "c"] {
        cache.put(&format!("0/b/{id}"), id.repeat(4).as_bytes());
    }
    assert_eq!(on_disk(), 8);
    assert_eq!(cache.get("0/b/a"), None);
    assert_eq!(cache.get("0/b/b").unwrap(), b"bbbb");
    cache.put("0/b/d", b"dddd");
    assert_eq!(cache.get("0/b/c"), None);
    assert_eq!(cache.get("0/b/b").unwrap(), b"bbbb");
    cache.put("0/b/e", &[0; 11]);
    assert_eq!(cache.get("0/b/e"), None);
    cache.delete("0/b/b");
    assert_eq!(cache.get("0/b/b"), None);
    assert_eq!(on_disk(), 4);
}
