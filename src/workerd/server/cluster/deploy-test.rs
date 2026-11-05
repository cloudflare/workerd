// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::path::Path;
use std::rc::Rc;

use futures::executor::block_on;
use workerd_capnp::config;

use super::CATALOG;
use super::Catalog;
use super::Current;
use super::FORMAT;
use super::MAX_CONFIG;
use super::Published;
use super::catalog;
use super::check_name;
use super::load;
use super::publish;
use super::read;
use super::upload;
use super::version_of;
use crate::Bucket;
use crate::Error;
use crate::Result;
use crate::Version;
use crate::bucket::Entry;
use crate::bucket::Guard;
use crate::bucket::Payload;
use crate::bucket::Store;
use crate::bucket::memory::Memory;
use crate::compose::deployment_config;

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

fn refusal<T: std::fmt::Debug>(result: Result<T>) -> String {
    match result {
        Err(Error::Deploy(message)) => message,
        other => panic!("expected a refusal, got {other:?}"),
    }
}

/// What a write does before or instead of reaching the store.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Fault {
    /// The write lands, and its answer is lost.
    Lands,
    /// The write fails without landing.
    Fails,
}

/// A store whose next catalog write is preceded by another deploy's (`race`), and with
/// `contend` every one is preceded by a rewrite of the catalog. The next write of the key of
/// `fault` has that fault, and when it lands, another deploy's catalog (`follow`) replaces it.
#[derive(Default)]
struct Flaky {
    inner: Memory,
    race: RefCell<Option<Vec<u8>>>,
    fault: RefCell<Option<(String, Fault)>>,
    follow: RefCell<Option<Vec<u8>>>,
    contend: Cell<bool>,
    catalog_writes: Cell<u32>,
    /// A key whose reads return a body-too-large error.
    too_large: RefCell<Option<String>>,
    /// A key whose reads fail.
    unreachable: RefCell<Option<String>>,
}

impl Flaky {
    fn fail(&self, key: &str, fault: Fault) {
        *self.fault.borrow_mut() = Some((key.to_owned(), fault));
    }
}

#[async_trait::async_trait(?Send)]
impl Store for Rc<Flaky> {
    async fn get(&self, key: &str, to: Option<&Path>) -> Result<Option<(Vec<u8>, Version)>> {
        if self.too_large.borrow().as_deref() == Some(key) {
            return Err(Error::TooLarge(format!("GET {key}")));
        }
        if self.unreachable.borrow().as_deref() == Some(key) {
            return Err(Error::Http(format!("GET {key}: 503")));
        }
        self.inner.get(key, to).await
    }

    async fn put(
        &self,
        key: &str,
        body: Payload,
        guard: Option<Guard>,
        metadata: &[(&str, String)],
    ) -> Result<Option<Version>> {
        if key == CATALOG {
            self.catalog_writes.set(self.catalog_writes.get() + 1);
            let race = self.race.borrow_mut().take();
            if let Some(race) = race {
                let race = Payload::Bytes(race.into());
                self.inner.put(key, race, None, &[]).await?;
            }
            if self.contend.get()
                && let Some((current, _)) = self.inner.get(key, None).await?
            {
                let current = Payload::Bytes(current.into());
                self.inner.put(key, current, None, &[]).await?;
            }
        }
        let fault = self.fault.borrow_mut().take_if(|(faulty, _)| faulty == key);
        match fault.map(|(_, fault)| fault) {
            Some(Fault::Lands) => {
                self.inner.put(key, body, guard, metadata).await?;
                let follow = self.follow.borrow_mut().take();
                if let Some(follow) = follow {
                    let follow = Payload::Bytes(follow.into());
                    self.inner.put(key, follow, None, &[]).await?;
                }
                Err(Error::Http("PUT timed out".into()))
            }
            Some(Fault::Fails) => Err(Error::Http("PUT timed out".into())),
            None => self.inner.put(key, body, guard, metadata).await,
        }
    }

    async fn list(&self, prefix: &str) -> Result<Vec<Entry>> {
        self.inner.list(prefix).await
    }

    async fn delete(&self, key: &str) -> Result<()> {
        self.inner.delete(key).await
    }
}

async fn catalog_of(bucket: &Bucket) -> Catalog {
    catalog(bucket).await.unwrap().unwrap().0
}

/// A catalog of revision `revision` that names `version` of deployment `name`.
fn naming(revision: u64, name: &str, version: String) -> Catalog {
    let current = Current {
        version,
        extra: serde_json::Map::new(),
    };
    Catalog {
        format: FORMAT,
        revision,
        deployments: BTreeMap::from([(name.to_owned(), current)]),
        extra: serde_json::Map::new(),
    }
}

#[test]
fn a_published_deployment_loads() {
    block_on(async {
        let bucket = Bucket::memory("");
        assert!(load(&bucket).await.unwrap().is_none());

        let config = services(&["app"]);
        let canonical = deployment_config("app", &config).unwrap();
        let version = version_of(&canonical);
        let published = publish(&bucket, "app", &config).await.unwrap();
        assert_eq!(
            published,
            Published {
                revision: 1,
                version: version.clone()
            }
        );

        let keys: Vec<_> = bucket
            .list("")
            .await
            .unwrap()
            .into_iter()
            .map(|e| e.key)
            .collect();
        assert_eq!(
            keys,
            [
                format!("deploy/app/{version}/config.bin"),
                format!("deploy/app/{version}/manifest.json"),
                CATALOG.to_owned(),
            ]
        );
        let manifest = bucket
            .get(&format!("deploy/app/{version}/manifest.json"))
            .await
            .unwrap()
            .unwrap()
            .0;
        let manifest: serde_json::Value = serde_json::from_slice(&manifest).unwrap();
        assert_eq!(
            manifest,
            serde_json::json!({"format": 1, "name": "app", "version": version, "requires": []})
        );
        let catalog = bucket.get(CATALOG).await.unwrap().unwrap().0;
        let catalog: serde_json::Value = serde_json::from_slice(&catalog).unwrap();
        assert_eq!(
            catalog,
            serde_json::json!({
                "format": 1,
                "revision": 1,
                "deployments": {"app": {"version": version}},
            })
        );

        let loaded = load(&bucket).await.unwrap().unwrap();
        assert_eq!(loaded.revision, 1);
        assert_eq!(loaded.deployments.len(), 1);
        assert_eq!(loaded.deployments[0].name, "app");
        assert_eq!(loaded.deployments[0].version, version);
        assert_eq!(loaded.deployments[0].config, canonical);
    });
}

#[test]
fn publishing_the_current_version_again_changes_nothing() {
    block_on(async {
        let bucket = Bucket::memory("");
        let first = publish(&bucket, "app", &services(&["app"])).await.unwrap();
        let again = publish(&bucket, "app", &services(&["app"])).await.unwrap();
        assert_eq!(first, again);
        assert_eq!(catalog_of(&bucket).await.revision, 1);
    });
}

#[test]
fn earlier_versions_stay_readable() {
    block_on(async {
        let bucket = Bucket::memory("");
        let first = publish(&bucket, "app", &services(&["app"])).await.unwrap();
        let second = publish(&bucket, "app", &services(&["app", "api"]))
            .await
            .unwrap();
        assert_eq!(second.revision, 2);
        assert_ne!(first.version, second.version);
        let old = read(&bucket, "app", &first.version).await.unwrap();
        assert_eq!(old.version, first.version);

        // A rollback is a deploy of the earlier version.
        let back = publish(&bucket, "app", &services(&["app"])).await.unwrap();
        assert_eq!(back.version, first.version);
        assert_eq!(back.revision, 3);
    });
}

#[test]
fn deployments_compose_and_conflicts_are_refused() {
    block_on(async {
        let bucket = Bucket::memory("");
        publish(&bucket, "a", &services(&["one"])).await.unwrap();
        publish(&bucket, "b", &services(&["two"])).await.unwrap();
        // A new version of a deployment may keep its own services.
        publish(&bucket, "a", &services(&["one", "three"]))
            .await
            .unwrap();
        let before = catalog_of(&bucket).await;

        let refused = refusal(publish(&bucket, "c", &services(&["two"])).await);
        assert_eq!(
            refused,
            "deployment \"c\" defines service \"two\". The same service is already defined by \
             deployment \"b\"."
        );
        assert_eq!(catalog_of(&bucket).await, before);
        // Refused before it was uploaded.
        assert!(bucket.list("deploy/c/").await.unwrap().is_empty());

        let names: Vec<_> = load(&bucket)
            .await
            .unwrap()
            .unwrap()
            .deployments
            .into_iter()
            .map(|d| d.name)
            .collect();
        assert_eq!(names, ["a", "b"]);
    });
}

#[test]
fn a_deployment_with_more_than_services_is_not_uploaded() {
    block_on(async {
        let bucket = Bucket::memory("");
        let mut message = capnp::message::Builder::new_default();
        let mut root = message.init_root::<config::Builder<'_>>();
        root.reborrow().init_services(0);
        root.init_autogates(1).set(0, "gate");
        let config = capnp::serialize::write_message_to_words(&message);
        assert!(refusal(publish(&bucket, "app", &config).await).contains("sets `autogates`"));
        assert!(bucket.list("").await.unwrap().is_empty());
    });
}

/// A deploy that loses the race for the catalog checks against what the winner published.
#[test]
fn concurrent_deploys_cannot_both_add_a_service() {
    block_on(async {
        let store = Rc::new(Flaky::default());
        let other = deployment_config("other", &services(&["shared"])).unwrap();
        let race = naming(1, "other", version_of(&other));
        *store.race.borrow_mut() = Some(serde_json::to_vec(&race).unwrap());
        let bucket = Bucket::new(store.clone(), "");
        upload(&bucket, "other", &other).await.unwrap();

        let refused = refusal(publish(&bucket, "app", &services(&["shared"])).await);
        assert_eq!(
            refused,
            "deployment \"app\" defines service \"shared\". The same service is already defined \
             by deployment \"other\"."
        );
        assert_eq!(catalog_of(&bucket).await, race);
    });
}

#[test]
fn a_deploy_that_loses_the_race_retries_on_the_winners_catalog() {
    block_on(async {
        let store = Rc::new(Flaky::default());
        let other = deployment_config("other", &services(&["one"])).unwrap();
        let race = naming(1, "other", version_of(&other));
        *store.race.borrow_mut() = Some(serde_json::to_vec(&race).unwrap());
        let bucket = Bucket::new(store.clone(), "");
        upload(&bucket, "other", &other).await.unwrap();

        let published = publish(&bucket, "app", &services(&["two"])).await.unwrap();
        assert_eq!(published.revision, 2);
        let names: Vec<_> = catalog_of(&bucket).await.deployments.into_keys().collect();
        assert_eq!(names, ["app", "other"]);
    });
}

#[test]
fn an_ambiguous_catalog_write_is_resolved_by_reading_it() {
    block_on(async {
        let store = Rc::new(Flaky::default());
        store.fail(CATALOG, Fault::Lands);
        let bucket = Bucket::new(store.clone(), "");
        let published = publish(&bucket, "app", &services(&["app"])).await.unwrap();
        assert_eq!(published.revision, 1);
        assert_eq!(catalog_of(&bucket).await.revision, 1);
        assert_eq!(store.catalog_writes.get(), 1);
    });
    block_on(async {
        let store = Rc::new(Flaky::default());
        store.fail(CATALOG, Fault::Fails);
        let bucket = Bucket::new(store.clone(), "");
        let published = publish(&bucket, "app", &services(&["app"])).await.unwrap();
        assert_eq!(published.revision, 1);
        assert_eq!(store.catalog_writes.get(), 2);
    });
}

/// A write that may have landed and that another deploy followed is not written again: it
/// would undo that deploy.
#[test]
fn an_ambiguous_catalog_write_does_not_undo_a_later_deploy() {
    block_on(async {
        let store = Rc::new(Flaky::default());
        let bucket = Bucket::new(store.clone(), "");
        let later = deployment_config("app", &services(&["later"])).unwrap();
        let later = naming(2, "app", upload(&bucket, "app", &later).await.unwrap());
        *store.follow.borrow_mut() = Some(serde_json::to_vec(&later).unwrap());
        store.fail(CATALOG, Fault::Lands);
        let refused = refusal(publish(&bucket, "app", &services(&["app"])).await);
        assert!(
            refused.ends_with(
                "may have been published. Workerd could not confirm the result because bucket: \
                 PUT timed out. deploy/catalog.json is at revision 2 and does not name this \
                 version."
            ),
            "{refused}"
        );
        assert_eq!(catalog_of(&bucket).await, later);
        assert_eq!(store.catalog_writes.get(), 1);
    });
}

#[test]
fn an_ambiguous_write_of_a_versions_object_is_resolved_by_reading_it() {
    for fault in [Fault::Lands, Fault::Fails] {
        block_on(async {
            let store = Rc::new(Flaky::default());
            let bucket = Bucket::new(store.clone(), "");
            let config = deployment_config("app", &services(&["app"])).unwrap();
            let version = version_of(&config);
            let key = format!("deploy/app/{version}/config.bin");
            store.fail(&key, fault);
            let result = upload(&bucket, "app", &config).await;
            if fault == Fault::Lands {
                assert_eq!(result.unwrap(), version);
                assert!(read(&bucket, "app", &version).await.is_ok());
            } else {
                assert!(matches!(result, Err(Error::Http(_))));
                assert!(bucket.list("").await.unwrap().is_empty());
            }
        });
    }
}

#[test]
fn a_bucket_that_ignores_conditional_writes_is_not_published_to() {
    block_on(async {
        let lax = Memory::default();
        lax.ignores_guards.set(true);
        let bucket = Bucket::new(lax, "");
        let result = publish(&bucket, "app", &services(&["app"])).await;
        assert!(matches!(result, Err(Error::Config(_))), "{result:?}");
        assert!(bucket.list("").await.unwrap().is_empty());
    });
}

#[test]
fn a_publish_keeps_the_catalogs_fields_it_does_not_know() {
    block_on(async {
        let bucket = Bucket::memory("");
        publish(&bucket, "a", &services(&["one"])).await.unwrap();
        let mut catalog: serde_json::Value =
            serde_json::from_slice(&bucket.get(CATALOG).await.unwrap().unwrap().0).unwrap();
        catalog["owner"] = serde_json::json!("ops");
        catalog["deployments"]["a"]["note"] = serde_json::json!("canary");
        bucket
            .put(CATALOG, serde_json::to_vec(&catalog).unwrap(), &[])
            .await
            .unwrap();
        publish(&bucket, "b", &services(&["two"])).await.unwrap();
        publish(&bucket, "a", &services(&["one", "three"]))
            .await
            .unwrap();
        let catalog: serde_json::Value =
            serde_json::from_slice(&bucket.get(CATALOG).await.unwrap().unwrap().0).unwrap();
        assert_eq!(catalog["revision"], 3);
        assert_eq!(catalog["owner"], "ops");
        assert_eq!(catalog["deployments"]["a"]["note"], "canary");
    });
}

#[test]
fn a_deploy_that_keeps_losing_the_race_gives_up() {
    block_on(async {
        let store = Rc::new(Flaky::default());
        let bucket = Bucket::new(store.clone(), "");
        publish(&bucket, "a", &services(&["one"])).await.unwrap();
        let before = catalog_of(&bucket).await;
        store.contend.set(true);
        let result = publish(&bucket, "b", &services(&["two"])).await;
        assert!(matches!(result, Err(Error::Contended(key)) if key == CATALOG));
        assert_eq!(catalog_of(&bucket).await, before);
    });
}

#[test]
fn an_unreadable_deployment_refuses_a_publish_and_is_named() {
    block_on(async {
        let bucket = Bucket::memory("");
        let published = publish(&bucket, "app", &services(&["app"])).await.unwrap();
        let key = format!("deploy/app/{}/manifest.json", published.version);
        bucket.delete(&key).await.unwrap();
        let before = catalog_of(&bucket).await;

        let refused = refusal(publish(&bucket, "other", &services(&["other"])).await);
        assert_eq!(
            refused,
            format!(
                "cannot publish deployment \"other\": deployment \"app\" at version {} could \
                 not be read to check for conflicts: {key} is missing",
                published.version
            )
        );
        assert_eq!(catalog_of(&bucket).await, before);
    });
}

/// Loads unreadable deployment "app" beside readable deployment "other" and returns the error.
async fn unreadable_app(bucket: &Bucket) -> String {
    let loaded = load(bucket).await.unwrap().unwrap();
    let names: Vec<_> = loaded.deployments.iter().map(|d| d.name.as_str()).collect();
    assert_eq!(names, ["other"]);
    let [unreadable] = &loaded.unreadable[..] else {
        panic!(
            "expected one unreadable deployment, got {:?}",
            loaded.unreadable
        );
    };
    assert_eq!(unreadable.name, "app");
    unreadable.error.clone()
}

#[test]
fn a_node_skips_a_deployment_it_cannot_read() {
    block_on(async {
        let bucket = Bucket::memory("");
        let published = publish(&bucket, "app", &services(&["app"])).await.unwrap();
        publish(&bucket, "other", &services(&["other"]))
            .await
            .unwrap();
        let prefix = format!("deploy/app/{}", published.version);

        // The node skips a deployment whose config.bin does not hash to its version.
        let config = bucket
            .get(&format!("{prefix}/config.bin"))
            .await
            .unwrap()
            .unwrap()
            .0;
        let key = format!("{prefix}/config.bin");
        bucket.put(&key, services(&["changed"]), &[]).await.unwrap();
        assert_eq!(
            unreadable_app(&bucket).await,
            format!(
                "the SHA-256 hash of {key} does not match version {}.",
                published.version
            )
        );
        bucket.put(&key, config, &[]).await.unwrap();
        assert!(load(&bucket).await.unwrap().unwrap().unreadable.is_empty());

        // The node skips a deployment whose manifest requires a feature or is missing.
        let key = format!("{prefix}/manifest.json");
        let manifest = bucket.get(&key).await.unwrap().unwrap().0;
        let mut requiring: serde_json::Value = serde_json::from_slice(&manifest).unwrap();
        requiring["requires"] = serde_json::json!(["teleportation"]);
        bucket
            .put(&key, serde_json::to_vec(&requiring).unwrap(), &[])
            .await
            .unwrap();
        assert_eq!(
            unreadable_app(&bucket).await,
            format!("{key} requires features this workerd does not support: teleportation.")
        );
        bucket.delete(&key).await.unwrap();
        assert_eq!(unreadable_app(&bucket).await, format!("{key} is missing"));
        bucket.put(&key, manifest, &[]).await.unwrap();

        // The node skips a deployment whose version in the catalog is not a SHA-256 hex string.
        let catalog: serde_json::Value =
            serde_json::from_slice(&bucket.get(CATALOG).await.unwrap().unwrap().0).unwrap();
        let mut odd = catalog.clone();
        odd["deployments"]["app"]["version"] = serde_json::json!("../other");
        bucket
            .put(CATALOG, serde_json::to_vec(&odd).unwrap(), &[])
            .await
            .unwrap();
        assert_eq!(
            unreadable_app(&bucket).await,
            "\"../other\" is not a valid version for deployment \"app\"."
        );

        // The node exits on a catalog of a later format.
        let mut later = catalog;
        later["format"] = serde_json::json!(2);
        bucket
            .put(CATALOG, serde_json::to_vec(&later).unwrap(), &[])
            .await
            .unwrap();
        assert_eq!(
            refusal(load(&bucket).await),
            "deploy/catalog.json uses format 2. This workerd supports format 1."
        );
    });
}

#[test]
fn a_node_skips_a_deployment_it_cannot_fetch_or_with_an_invalid_name() {
    block_on(async {
        let store = Rc::new(Flaky::default());
        let bucket = Bucket::new(Rc::clone(&store), "");
        let published = publish(&bucket, "app", &services(&["app"])).await.unwrap();
        publish(&bucket, "other", &services(&["other"]))
            .await
            .unwrap();

        let key = format!("deploy/app/{}/config.bin", published.version);
        *store.too_large.borrow_mut() = Some(key.clone());
        assert_eq!(
            unreadable_app(&bucket).await,
            format!("bucket: GET {key}: response body too large")
        );
        *store.too_large.borrow_mut() = None;

        // A failed request for a deployment's object skips only that deployment.
        let key = format!("deploy/app/{}/manifest.json", published.version);
        *store.unreachable.borrow_mut() = Some(key.clone());
        assert_eq!(
            unreadable_app(&bucket).await,
            format!("bucket: GET {key}: 503")
        );
        *store.unreachable.borrow_mut() = None;

        // An entry whose name is not a deployment name is skipped, and the rest load.
        let mut catalog: serde_json::Value =
            serde_json::from_slice(&bucket.get(CATALOG).await.unwrap().unwrap().0).unwrap();
        catalog["deployments"]["../evil"] = serde_json::json!({ "version": published.version });
        bucket
            .put(CATALOG, serde_json::to_vec(&catalog).unwrap(), &[])
            .await
            .unwrap();
        let loaded = load(&bucket).await.unwrap().unwrap();
        let names: Vec<_> = loaded.deployments.iter().map(|d| d.name.as_str()).collect();
        assert_eq!(names, ["app", "other"]);
        let [unreadable] = &loaded.unreadable[..] else {
            panic!(
                "expected one unreadable deployment, got {:?}",
                loaded.unreadable
            );
        };
        assert_eq!(unreadable.name, "../evil");
        assert!(
            unreadable
                .error
                .starts_with("\"../evil\" is not a valid deployment name."),
            "{}",
            unreadable.error
        );
    });
}

#[test]
fn an_object_of_a_version_is_never_rewritten() {
    block_on(async {
        let bucket = Bucket::memory("");
        let config = deployment_config("app", &services(&["app"])).unwrap();
        let version = upload(&bucket, "app", &config).await.unwrap();
        assert_eq!(upload(&bucket, "app", &config).await.unwrap(), version);

        let key = format!("deploy/app/{version}/config.bin");
        bucket.put(&key, b"other".to_vec(), &[]).await.unwrap();
        assert_eq!(
            refusal(upload(&bucket, "app", &config).await),
            format!("{key} already exists with different content.")
        );
    });
}

#[test]
fn a_deployment_too_large_for_a_node_to_read_is_refused() {
    block_on(async {
        let bucket = Bucket::memory("");
        let big = vec![0; MAX_CONFIG + 1];
        assert!(refusal(upload(&bucket, "app", &big).await).contains("The maximum size"));
    });
}

#[test]
fn deployment_names_are_restricted() {
    for name in ["app", "my-app_2", "A", &"a".repeat(64)] {
        assert!(check_name(name).is_ok(), "{name}");
    }
    for name in [
        "",
        "-app",
        "_app",
        "a.b",
        "a/b",
        "catalog.json",
        &"a".repeat(65),
    ] {
        assert!(check_name(name).is_err(), "{name}");
    }
}
