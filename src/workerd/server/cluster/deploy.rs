// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Deployments: named applications kept in the bucket.
//!
//! Every node serves the deployments the catalog names beside its own config (`compose.rs` has
//! the rules they compose by). Any tool may write this layout, not only workerd; under the
//! cluster's `v1/`:
//!
//! - `deploy/<name>/<version>/config.bin`: an encoded `Config` message that holds only
//!   `services`, worker modules embedded. `<version>` is the SHA-256 of these bytes in lowercase
//!   hex, so a version names its content. workerd writes the message in canonical form, so equal
//!   content gets an equal version; a reader requires only that the bytes hash to the version.
//! - `deploy/<name>/<version>/manifest.json`, written after `config.bin`:
//!   `{"format": 1, "name": "<name>", "version": "<version>", "requires": []}`. `requires` lists
//!   features a node must support to load the deployment; format 1 defines none, so a node
//!   refuses a manifest that lists any.
//! - `deploy/catalog.json`: `{"format": 1, "revision": <n>, "deployments": {"<name>":
//!   {"version": "<version>"}, ...}}`, the current version of every named deployment. It is the
//!   one object a deploy replaces, with a conditional write (`If-None-Match: *` while it is
//!   absent, `If-Match` after), so a deploy publishes all of its services or none, and a deploy
//!   checked against one catalog cannot replace another. `revision` goes up by one each time.
//!   A deploy keeps the fields of the catalog and of its entries that it does not know.
//! - `deploy/roll.json`: the nodes' turns at restarting on a new revision, which they write
//!   (`rollout.rs`).
//!
//! A name is 1 to 64 ASCII letters, digits, `-` and `_`, starting with a letter or digit. A
//! version's objects are never rewritten or removed when the catalog moves on, so a rollback is
//! a deploy of an earlier version, or, by hand, an edit of `catalog.json` that names it and
//! raises `revision`. A node refuses a catalog or a manifest whose format it does not read. It
//! reads the catalog and the deployments it names at startup, and adopts a later revision by
//! restarting (`rollout.rs`).
//!
//! A deployment runs with the authority of each node's config: its services reach the node's
//! network, environment variables, container engine and named services. Whoever may write
//! `deploy/` has that authority on every node; limiting a writer to `deploy/` (with an IAM policy
//! on `v1/deploy/`, say) does not limit it.

use std::collections::BTreeMap;
use std::collections::HashMap;
use std::collections::hash_map::Entry;

use ring::rand::SecureRandom;
use ring::rand::SystemRandom;
use serde::Deserialize;
use serde::Serialize;

use crate::Bucket;
use crate::Error;
use crate::Guard;
use crate::Result;
use crate::compose;
use crate::hex;
use crate::sha256;

/// The format of the catalog and the manifests this build reads and writes.
pub const FORMAT: u32 = 1;

pub const CATALOG: &str = "deploy/catalog.json";

/// The largest `config.bin`: the most of a response the bucket client holds in memory.
pub const MAX_CONFIG: usize = crate::http::MAX_BODY;

/// Catalog writes a publish retries when other deploys keep replacing the catalog first.
const PUBLISH_ATTEMPTS: usize = 8;

/// `deploy/catalog.json`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Catalog {
    pub format: u32,
    pub revision: u64,
    pub deployments: BTreeMap<String, Current>,
    /// Fields of a later revision of the format, kept as they are.
    #[serde(flatten)]
    pub extra: serde_json::Map<String, serde_json::Value>,
}

/// A deployment's entry in the catalog.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Current {
    pub version: String,
    #[serde(flatten)]
    pub extra: serde_json::Map<String, serde_json::Value>,
}

/// `deploy/<name>/<version>/manifest.json`.
#[derive(Debug, PartialEq, Eq, Serialize, Deserialize)]
struct Manifest {
    format: u32,
    name: String,
    version: String,
    #[serde(default)]
    requires: Vec<String>,
}

/// One version of a deployment: `config` is its `config.bin`.
#[derive(Clone, Debug)]
pub struct Deployment {
    pub name: String,
    pub version: String,
    pub config: Vec<u8>,
}

/// The deployments a catalog names, in order of name.
#[derive(Debug)]
pub struct Loaded {
    pub revision: u64,
    pub deployments: Vec<Deployment>,
}

/// What a publish left in the catalog.
#[derive(Debug, PartialEq, Eq)]
pub struct Published {
    pub revision: u64,
    pub version: String,
}

/// The version of a deployment whose `config.bin` is `config`.
pub fn version_of(config: &[u8]) -> String {
    hex(&sha256(config))
}

pub fn check_name(name: &str) -> Result<()> {
    let mut chars = name.chars();
    let valid = name.len() <= 64
        && chars.next().is_some_and(|c| c.is_ascii_alphanumeric())
        && chars.all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_');
    if valid {
        Ok(())
    } else {
        Err(Error::Deploy(format!(
            "\"{name}\" is not a deployment name: 1 to 64 letters, digits, '-' and '_', \
             starting with a letter or digit"
        )))
    }
}

fn prefix(name: &str, version: &str) -> String {
    format!("deploy/{name}/{version}")
}

/// The catalog and its version, or `None` before the first deploy.
pub async fn catalog(bucket: &Bucket) -> Result<Option<(Catalog, crate::Version)>> {
    let Some((body, version)) = bucket.get(CATALOG).await? else {
        return Ok(None);
    };
    let catalog: Catalog = serde_json::from_slice(&body)
        .map_err(|e| Error::Deploy(format!("{CATALOG} does not parse: {e}")))?;
    check_format(CATALOG, catalog.format)?;
    for name in catalog.deployments.keys() {
        check_name(name)?;
    }
    Ok(Some((catalog, version)))
}

/// Every deployment the catalog names, or `None` when there is no catalog.
pub async fn load(bucket: &Bucket) -> Result<Option<Loaded>> {
    let Some((catalog, _)) = catalog(bucket).await? else {
        return Ok(None);
    };
    let mut deployments = Vec::with_capacity(catalog.deployments.len());
    for (name, current) in &catalog.deployments {
        deployments.push(read(bucket, name, &current.version).await?);
    }
    Ok(Some(Loaded {
        revision: catalog.revision,
        deployments,
    }))
}

/// Version `version` of deployment `name`, checked against its manifest and its hash.
pub async fn read(bucket: &Bucket, name: &str, version: &str) -> Result<Deployment> {
    check_name(name)?;
    let is_hash = version.len() == 64
        && version
            .bytes()
            .all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'));
    if !is_hash {
        return Err(Error::Deploy(format!(
            "deployment \"{name}\": \"{version}\" is not a version"
        )));
    }
    let prefix = prefix(name, version);
    let key = format!("{prefix}/manifest.json");
    let (body, _) = bucket
        .get(&key)
        .await?
        .ok_or_else(|| Error::Deploy(format!("{key} is missing")))?;
    let manifest: Manifest = serde_json::from_slice(&body)
        .map_err(|e| Error::Deploy(format!("{key} does not parse: {e}")))?;
    check_format(&key, manifest.format)?;
    if manifest.name != name || manifest.version != version {
        return Err(Error::Deploy(format!(
            "{key} describes {}/{}",
            manifest.name, manifest.version
        )));
    }
    if !manifest.requires.is_empty() {
        return Err(Error::Deploy(format!(
            "{key} requires {}, which this workerd does not support",
            manifest.requires.join(", ")
        )));
    }
    let key = format!("{prefix}/config.bin");
    let (config, _) = bucket
        .get(&key)
        .await?
        .ok_or_else(|| Error::Deploy(format!("{key} is missing")))?;
    if version_of(&config) != version {
        return Err(Error::Deploy(format!("{key} does not hash to its version")));
    }
    Ok(Deployment {
        name: name.to_owned(),
        version: version.to_owned(),
        config,
    })
}

/// Writes the immutable objects of a version of deployment `name` and answers the version.
///
/// `config.bin` goes first, then the manifest. `config` must already be what `config.bin`
/// holds (`compose::deployment_config`). Objects already there are checked, not rewritten: the
/// version names their content.
pub async fn upload(bucket: &Bucket, name: &str, config: &[u8]) -> Result<String> {
    check_name(name)?;
    if config.len() > MAX_CONFIG {
        return Err(Error::Deploy(format!(
            "deployment \"{name}\" is {} bytes; the most a deployment can be is {MAX_CONFIG}",
            config.len()
        )));
    }
    let version = version_of(config);
    let prefix = prefix(name, &version);
    let key = format!("{prefix}/config.bin");
    write_once(bucket, &key, config.to_vec(), |existing| existing == config).await?;
    let manifest = Manifest {
        format: FORMAT,
        name: name.to_owned(),
        version: version.clone(),
        requires: Vec::new(),
    };
    let key = format!("{prefix}/manifest.json");
    let body = serde_json::to_vec_pretty(&manifest)?;
    write_once(bucket, &key, body, |existing| {
        serde_json::from_slice::<Manifest>(existing).is_ok_and(|existing| existing == manifest)
    })
    .await?;
    Ok(version)
}

/// Creates `key`, or checks with `same` that what is there already is the same.
async fn write_once(
    bucket: &Bucket,
    key: &str,
    body: Vec<u8>,
    same: impl Fn(&[u8]) -> bool,
) -> Result<()> {
    let failed = match bucket.put_if(key, body, Guard::Absent).await {
        Ok(Some(_)) => return Ok(()),
        Ok(None) => None,
        // The write may have landed: what is there says.
        Err(error) => Some(error),
    };
    match bucket.get(key).await? {
        Some((existing, _)) if same(&existing) => Ok(()),
        Some(_) => Err(Error::Deploy(format!("{key} exists with other content"))),
        None => Err(failed.unwrap_or_else(|| {
            // Deleted since, or a store that answers a concurrent write with a conflict.
            Error::Contended(key.to_owned())
        })),
    }
}

/// Publishes `config`, an encoded `Config`, as deployment `name`.
///
/// Checks it, uploads it, and points the catalog at it, unless the catalog does already.
/// Refused, with the catalog unchanged, when it breaks a rule of `compose.rs` against the other
/// deployments of the catalog it would replace, or when the bucket ignores conditional writes.
pub async fn publish(bucket: &Bucket, name: &str, config: &[u8]) -> Result<Published> {
    check_name(name)?;
    let config = compose::deployment_config(name, config)?;
    check_conditional_writes(bucket).await?;
    let version = upload(bucket, name, &config).await?;
    let ours = Deployment {
        name: name.to_owned(),
        version: version.clone(),
        config,
    };
    let mut others: HashMap<(String, String), Deployment> = HashMap::new();
    for _ in 0..PUBLISH_ATTEMPTS {
        let base = catalog(bucket).await?;
        let base_version = base.as_ref().map(|(_, version)| version.clone());
        let (current, guard) = match base {
            Some((catalog, version)) => (catalog, Guard::Match(version)),
            None => (
                Catalog {
                    format: FORMAT,
                    revision: 0,
                    deployments: BTreeMap::new(),
                    extra: serde_json::Map::new(),
                },
                Guard::Absent,
            ),
        };
        if current.deployments.get(name).map(|c| &c.version) == Some(&version) {
            return Ok(Published {
                revision: current.revision,
                version,
            });
        }
        for (other, entry) in current.deployments.iter().filter(|(n, _)| *n != name) {
            let key = (other.clone(), entry.version.clone());
            if let Entry::Vacant(vacant) = others.entry(key) {
                vacant.insert(read(bucket, other, &entry.version).await?);
            }
        }
        let mut all: Vec<_> = current
            .deployments
            .iter()
            .filter(|(n, _)| *n != name)
            .filter_map(|(n, entry)| others.get(&(n.clone(), entry.version.clone())))
            .collect();
        all.push(&ours);
        compose::check_deployments(&all)?;

        let mut next = current;
        next.revision += 1;
        // An entry's fields this build does not know stay.
        next.deployments
            .entry(name.to_owned())
            .or_insert_with(|| Current {
                version: String::new(),
                extra: serde_json::Map::new(),
            })
            .version
            .clone_from(&version);
        let body = serde_json::to_vec_pretty(&next)?;
        let published = Published {
            revision: next.revision,
            version: version.clone(),
        };
        match bucket.put_if(CATALOG, body, guard).await {
            Ok(Some(_)) => return Ok(published),
            // Another deploy replaced it first: check against what that one published.
            Ok(None) => {}
            Err(error) => {
                if let Some(published) =
                    resolve(bucket, name, &version, base_version.as_ref(), error).await?
                {
                    return Ok(published);
                }
            }
        }
    }
    Err(Error::Contended(CATALOG.to_owned()))
}

/// Refuses a bucket that ignores conditional writes, on an object no other deploy writes: a `.`
/// is in no deployment's name.
async fn check_conditional_writes(bucket: &Bucket) -> Result<()> {
    let mut nonce = [0; 16];
    SecureRandom::fill(&SystemRandom::new(), &mut nonce)
        .map_err(|_| Error::Deploy("no random numbers".into()))?;
    let probe = format!("deploy/{}.probe", hex(&nonce));
    crate::cluster::check_conditional_writes(bucket, &probe).await
}

/// After a write of the catalog that failed with `error` and may have landed, the outcome:
/// `version` of `name` published, or `None` when the catalog is still `base` and the write can
/// be tried again. Once the catalog has moved on, writing again could undo a later deploy, so
/// whether this one landed stays unknown.
async fn resolve(
    bucket: &Bucket,
    name: &str,
    version: &str,
    base: Option<&crate::Version>,
    error: Error,
) -> Result<Option<Published>> {
    let Ok(now) = catalog(bucket).await else {
        return Err(error);
    };
    let state = match now {
        Some((now, _))
            if now
                .deployments
                .get(name)
                .is_some_and(|c| c.version == version) =>
        {
            return Ok(Some(Published {
                revision: now.revision,
                version: version.to_owned(),
            }));
        }
        now if now.as_ref().map(|(_, v)| v) == base => return Ok(None),
        Some((now, _)) => format!(
            "has since moved to revision {}, which does not name it",
            now.revision
        ),
        None => "has since been removed".to_owned(),
    };
    Err(Error::Deploy(format!(
        "deployment \"{name}\" version {version} may or may not have been published ({error}); \
         {CATALOG} {state}"
    )))
}

fn check_format(key: &str, format: u32) -> Result<()> {
    if format == FORMAT {
        Ok(())
    } else {
        Err(Error::Deploy(format!(
            "{key} has format {format}; this workerd reads format {FORMAT}"
        )))
    }
}

#[cfg(test)]
#[path = "deploy-test.rs"]
mod tests;
