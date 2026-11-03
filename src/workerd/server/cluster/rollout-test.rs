// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::RefCell;

use workerd_capnp::config;

use super::*;

fn block_on<F: std::future::Future>(f: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
        .enable_time()
        .build()
        .unwrap()
        .block_on(f)
}

/// An encoded `Config` of network services named `names`.
fn services(names: &[&str]) -> Vec<u8> {
    let mut message = capnp::message::Builder::new_default();
    let root = message.init_root::<config::Builder<'_>>();
    let mut list = root.init_services(names.len() as u32);
    for (index, name) in names.iter().enumerate() {
        let mut service = list.reborrow().get(index as u32);
        service.set_name(name);
        service.init_network();
    }
    capnp::serialize::write_message_to_words(&message)
}

#[expect(clippy::unnecessary_wraps, reason = "a `Check` that accepts anything")]
const fn accept(_: &[Deployment]) -> std::result::Result<(), String> {
    Ok(())
}

/// `me`'s turn on a listing of `live` taken now.
fn turn(watch: &mut Watch, me: &str, live: &[Node<'_>]) -> Turn {
    watch.turn(me, now_ms(), (Some(now_ms()), live), None)
}

const fn serving(node: &str) -> Node<'_> {
    Node {
        node,
        revision: Some(0),
        refused: None,
        failed: None,
    }
}

#[test]
fn a_record_names_the_revisions_served_refused_and_failed() {
    block_on(async {
        let bucket = Bucket::memory("");
        let a = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        let now = now_ms();
        a.renew(now).await.unwrap();
        let live = live_nodes(&bucket, &|| now).await.unwrap();
        assert_eq!((live[0].revision, live[0].refused), (None, None));
        // Written as before, without the fields, until the node serves.
        let (body, _) = bucket.get("nodes/a.json").await.unwrap().unwrap();
        assert!(!String::from_utf8(body).unwrap().contains("revision"));

        a.serve(3);
        a.refuse(4);
        a.fail(5);
        a.renew(now).await.unwrap();
        let live = live_nodes(&bucket, &|| now).await.unwrap();
        assert_eq!(
            (live[0].revision, live[0].refused, live[0].failed),
            (Some(3), Some(4), Some(5))
        );
        assert!(progress(&live, 3).done(1));
        assert_eq!(progress(&live, 4).refused, ["a"]);
        assert_eq!(progress(&live, 5).failed, ["a"]);
        // An expired lease is not live.
        assert!(
            live_nodes(&bucket, &|| now + 10_000)
                .await
                .unwrap()
                .is_empty()
        );
    });
}

#[test]
fn a_waiting_deploy_takes_a_roll_that_does_not_parse_as_none() {
    block_on(async {
        let bucket = Bucket::memory("");
        assert_eq!(roll(&bucket).await.unwrap(), None);
        bucket.put(ROLL, b"{ not json".to_vec(), &[]).await.unwrap();
        assert_eq!(roll(&bucket).await.unwrap(), None);
        let file = br#"{"revision": 2, "baseline": 3, "since_ms": 0, "done": true}"#;
        bucket.put(ROLL, file.to_vec(), &[]).await.unwrap();
        let read = roll(&bucket).await.unwrap().unwrap();
        assert_eq!((read.revision, read.baseline, read.done), (2, 3, true));
    });
}

#[test]
fn a_new_revision_that_composes_is_adopted() {
    block_on(async {
        let bucket = Bucket::memory("");
        let registry = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        let mut watch = Watch::new(0);
        read(&bucket, &registry, &mut watch, 0, &accept).await;
        assert_eq!(watch.accepted(), None);

        let published = deploy::publish(&bucket, "app", &services(&["x"]))
            .await
            .unwrap();
        let seen = RefCell::new(Vec::new());
        let check = |deployments: &[Deployment]| {
            let names = deployments.iter().map(|d| d.name.clone());
            seen.borrow_mut().extend(names);
            Ok(())
        };
        read(&bucket, &registry, &mut watch, 0, &check).await;
        assert_eq!(*seen.borrow(), ["app"]);
        assert_eq!(watch.accepted(), Some(published.revision));
        // Checked once.
        read(&bucket, &registry, &mut watch, 0, &check).await;
        assert_eq!(seen.borrow().len(), 1);
    });
}

#[test]
fn a_revision_this_node_cannot_serve_is_refused_in_its_record() {
    block_on(async {
        let bucket = Bucket::memory("");
        let registry = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        let published = deploy::publish(&bucket, "app", &services(&["x"]))
            .await
            .unwrap();
        let mut watch = Watch::new(0);
        let refuse = |_: &[Deployment]| Err("service \"x\" is defined twice".to_owned());
        read(&bucket, &registry, &mut watch, 0, &refuse).await;
        assert_eq!(watch.refused(), Some(published.revision));
        assert_eq!(watch.accepted(), None);
        registry.renew(now_ms()).await.unwrap();
        let live = live_nodes(&bucket, &now_ms).await.unwrap();
        assert_eq!(live[0].refused, Some(published.revision));
    });
}

#[test]
fn a_catalog_naming_a_missing_version_is_refused_when_read_so_twice() {
    block_on(async {
        let bucket = Bucket::memory("");
        let registry = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        let version = "a".repeat(64);
        let catalog = format!(
            r#"{{"format": 1, "revision": 7, "deployments": {{"app": {{"version": "{version}"}}}}}}"#
        );
        bucket
            .put(deploy::CATALOG, catalog.into_bytes(), &[])
            .await
            .unwrap();
        let mut watch = Watch::new(6);
        read(&bucket, &registry, &mut watch, 6, &accept).await;
        // Possibly a bad read: again at the next.
        assert_eq!(watch.refused(), None);
        read(&bucket, &registry, &mut watch, 6, &accept).await;
        assert_eq!(watch.refused(), Some(7));
    });
}

#[test]
fn a_catalog_that_does_not_parse_is_served_through() {
    block_on(async {
        let bucket = Bucket::memory("");
        let registry = Registry::new(bucket.clone(), "a", "a:1", 10_000);
        bucket
            .put(deploy::CATALOG, b"{".to_vec(), &[])
            .await
            .unwrap();
        let mut watch = Watch::new(1);
        read(&bucket, &registry, &mut watch, 1, &accept).await;
        assert!(watch.failing());
        assert_eq!(watch.refused(), None);
        assert_eq!(watch.accepted(), None);
    });
}

#[test]
fn of_two_nodes_contending_for_a_turn_one_takes_it() {
    block_on(async {
        let bucket = Bucket::memory("");
        let live = [serving("a"), serving("b")];
        let (mut a, mut b) = (Watch::new(0), Watch::new(0));
        for watch in [&mut a, &mut b] {
            watch.polled(Found::Revision(1));
            watch.checked(1, Checked::Accept, now_ms() - 1);
        }
        // Both begin the roll from the same read; one write lands.
        let (mut seen_a, mut seen_b) = (Cached::default(), Cached::default());
        let Turn::Begin { roll: begun, .. } = turn(&mut a, "a", &live) else {
            panic!()
        };
        assert!(write(&bucket, &mut seen_a, &begun).await);
        assert!(!write(&bucket, &mut seen_b, &begun).await);
        assert_eq!(seen_b.roll.as_ref(), Some(&begun));

        // Both take the turn from the same read; one write lands.
        let mine = |watch: &mut Watch, me, seen: &Cached| {
            watch.turn(me, now_ms(), (Some(now_ms()), &live), seen.roll.as_ref())
        };
        let (Turn::Take(by_a), Turn::Take(by_b)) =
            (mine(&mut a, "a", &seen_a), mine(&mut b, "b", &seen_b))
        else {
            panic!()
        };
        assert!(write(&bucket, &mut seen_b, &by_b).await);
        assert!(!write(&bucket, &mut seen_a, &by_a).await);
        assert_eq!(mine(&mut a, "a", &seen_a), Turn::Held("b".into()));

        // The file is what a person may edit, too.
        let (body, _) = bucket.get(ROLL).await.unwrap().unwrap();
        let file: serde_json::Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(file["revision"], 1);
        assert_eq!(file["baseline"], 2);
        assert_eq!(file["holder"], "b");
    });
}
