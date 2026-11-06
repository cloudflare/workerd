// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Deployments: named applications kept in the bucket.
//!
//! Every node serves the deployments the catalog names beside its own config. `compose.rs` has
//! the rules they compose by. Any tool may write this layout. Under the cluster's `v1/`:
//!
//! - `deploy/<name>/<version>/config.bin`: an encoded `Config` that holds only `services`, with
//!   worker modules embedded. `<version>` is the SHA-256 of these bytes in lowercase hex.
//!   workerd writes the message in canonical form, so equal content gets an equal version. A
//!   reader only checks that the bytes hash to the version.
//! - `deploy/<name>/<version>/manifest.json`, written after `config.bin`:
//!   `{"format": 1, "name": "<name>", "version": "<version>", "requires": []}`. `requires` lists
//!   features a node must support to load the deployment. Format 1 defines none, so a node
//!   skips a deployment whose manifest lists any. Metadata about the version, such as who deployed
//!   it or its source revision, belongs here.
//! - `deploy/catalog.json`: `{"format": 1, "revision": <n>, "deployments": {"<name>":
//!   {"version": "<version>"}, ...}}`, the current version of every deployment. A deploy
//!   replaces it with a conditional write: `If-None-Match: *` while it is absent, `If-Match`
//!   after. So a deploy publishes all of its services or none, and a deploy checked against one
//!   catalog cannot replace another. `revision` goes up by one on each write.
//!
//! A deploy preserves catalog fields it does not know. It also preserves a deployment entry's
//! fields when changing the entry's version. Extra entry fields describe the deployment, not a
//! particular version.
//!
//! A name is 1 to 64 ASCII letters, digits, `-` and `_`, and starts with a letter or digit. A
//! version's objects are never rewritten or removed. To roll back, deploy an earlier version,
//! or edit `catalog.json` by hand to name it and raise `revision`. A node refuses a catalog whose
//! format it does not read. It skips an entry whose name is invalid and a deployment whose objects
//! are missing, invalid, or too large to read, including a manifest whose format it does not read.
//! It reads the catalog and its deployments once, at startup.
//!
//! A deployment runs with the authority of each node's config. Its services can reach the node's
//! network, container engine and named services. Whoever can write `deploy/` has that authority
//! on every node, even when an IAM policy limits the writer to `v1/deploy/`.

use std::collections::BTreeMap;
use std::collections::HashMap;
use std::collections::hash_map::Entry;

use futures::StreamExt;
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

/// Limits `load` to this many concurrent deployment reads.
const CONCURRENT_READS: usize = 8;

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

/// Holds the result of loading every deployment a catalog names, in order of name.
#[derive(Debug)]
pub struct Loaded {
    pub revision: u64,
    pub deployments: Vec<Deployment>,
    /// The deployments whose objects are missing or invalid and why, in order of name.
    pub unreadable: Vec<Unreadable>,
}

/// Describes a deployment whose objects are missing or invalid and why.
#[derive(Debug, PartialEq, Eq)]
pub struct Unreadable {
    pub name: String,
    pub version: String,
    pub error: String,
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
            "{name:?} is not a valid deployment name. Use 1 to 64 ASCII letters, digits, \
             hyphens, or underscores. Start with a letter or digit."
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
        .map_err(|e| Error::Deploy(format!("could not parse {CATALOG}: {e}")))?;
    check_format(CATALOG, catalog.format)?;
    Ok(Some((catalog, version)))
}

/// Loads every deployment the catalog names, in order, or returns `None` without a catalog.
///
/// It puts deployments with an invalid name and those whose objects are missing, invalid, or too
/// large in `Loaded::unreadable`. Only a failure to reach or read the catalog fails the load.
pub async fn load(bucket: &Bucket) -> Result<Option<Loaded>> {
    let Some((catalog, _)) = catalog(bucket).await? else {
        return Ok(None);
    };
    let reads = catalog
        .deployments
        .iter()
        .map(|(name, current)| async move {
            // Whatever keeps a deployment from being read, even a failed request, skips only
            // that deployment.
            read(bucket, name, &current.version)
                .await
                .map_err(|error| Unreadable {
                    name: name.clone(),
                    version: current.version.clone(),
                    error: error.to_string(),
                })
        });
    let results: Vec<_> = futures::stream::iter(reads)
        .buffered(CONCURRENT_READS)
        .collect()
        .await;
    let mut loaded = Loaded {
        revision: catalog.revision,
        deployments: Vec::new(),
        unreadable: Vec::new(),
    };
    for result in results {
        match result {
            Ok(deployment) => loaded.deployments.push(deployment),
            Err(unreadable) => loaded.unreadable.push(unreadable),
        }
    }
    Ok(Some(loaded))
}

/// Reads version `version` of deployment `name` and checks its manifest and hash.
pub async fn read(bucket: &Bucket, name: &str, version: &str) -> Result<Deployment> {
    check_name(name)?;
    let is_hash = version.len() == 64
        && version
            .bytes()
            .all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'));
    if !is_hash {
        return Err(Error::Deploy(format!(
            "\"{version}\" is not a valid version for deployment \"{name}\"."
        )));
    }
    let prefix = prefix(name, version);
    let key = format!("{prefix}/manifest.json");
    let (body, _) = bucket
        .get(&key)
        .await?
        .ok_or_else(|| Error::Deploy(format!("{key} is missing")))?;
    let manifest: Manifest = serde_json::from_slice(&body)
        .map_err(|e| Error::Deploy(format!("could not parse {key}: {e}")))?;
    check_format(&key, manifest.format)?;
    if manifest.name != name || manifest.version != version {
        return Err(Error::Deploy(format!(
            "{key} describes deployment \"{}\" version {}. Expected deployment \"{name}\" \
             version {version}.",
            manifest.name, manifest.version,
        )));
    }
    if !manifest.requires.is_empty() {
        return Err(Error::Deploy(format!(
            "{key} requires features this workerd does not support: {}.",
            manifest.requires.join(", ")
        )));
    }
    let key = format!("{prefix}/config.bin");
    let (config, _) = bucket
        .get(&key)
        .await?
        .ok_or_else(|| Error::Deploy(format!("{key} is missing")))?;
    if version_of(&config) != version {
        return Err(Error::Deploy(format!(
            "the SHA-256 hash of {key} does not match version {version}."
        )));
    }
    Ok(Deployment {
        name: name.to_owned(),
        version: version.to_owned(),
        config,
    })
}

/// Uploads the immutable objects for a version of deployment `name` and returns the version.
///
/// Writes `config.bin` before the manifest. `config` must already contain the exact bytes for
/// `config.bin`, as produced by `compose::deployment_config`. Existing objects are checked instead
/// of rewritten because the version names their content.
pub async fn upload(bucket: &Bucket, name: &str, config: &[u8]) -> Result<String> {
    check_name(name)?;
    if config.len() > MAX_CONFIG {
        return Err(Error::Deploy(format!(
            "deployment \"{name}\" is {} bytes. The maximum size is {MAX_CONFIG} bytes.",
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
        Some(_) => Err(Error::Deploy(format!(
            "{key} already exists with different content."
        ))),
        None => Err(failed.unwrap_or_else(|| {
            // Deleted since, or a store that answers a concurrent write with a conflict.
            Error::Contended(key.to_owned())
        })),
    }
}

/// Publishes `config`, an encoded `Config`, as deployment `name`.
///
/// Checks it against the catalog's other deployments before uploading it. The catalog points to
/// the uploaded deployment unless it already does. Every catalog write repeats the composition
/// check. The catalog stays unchanged if the deployment breaks a rule in `compose.rs` or the bucket
/// ignores conditional writes. A deployment refused by the first check is not uploaded.
pub async fn publish(bucket: &Bucket, name: &str, config: &[u8]) -> Result<Published> {
    check_name(name)?;
    let config = compose::deployment_config(name, config)?;
    let ours = Deployment {
        name: name.to_owned(),
        version: version_of(&config),
        config,
    };
    // The other deployments' versions read so far.
    let mut others: HashMap<(String, String), Deployment> = HashMap::new();
    // Unless the catalog names this version already, which changes nothing.
    if let Some((current, _)) = catalog(bucket).await?
        && current.deployments.get(name).map(|c| &c.version) != Some(&ours.version)
    {
        check_against(bucket, &current, &ours, &mut others).await?;
    }
    check_conditional_writes(bucket).await?;
    let version = upload(bucket, name, &ours.config).await?;
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
        check_against(bucket, &current, &ours, &mut others).await?;

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

/// Checks `ours` against the deployments `current` names other than it, by the rules of
/// `compose.rs`. `others` holds the versions read so far, and gains those read now. A deployment
/// that cannot be read refuses the publish, with an error that names it.
async fn check_against(
    bucket: &Bucket,
    current: &Catalog,
    ours: &Deployment,
    others: &mut HashMap<(String, String), Deployment>,
) -> Result<()> {
    let named = || current.deployments.iter().filter(|(n, _)| **n != ours.name);
    for (other, entry) in named() {
        let key = (other.clone(), entry.version.clone());
        if let Entry::Vacant(vacant) = others.entry(key) {
            let deployment = read(bucket, other, &entry.version).await.map_err(|error| {
                Error::Deploy(format!(
                    "cannot publish deployment \"{}\": deployment \"{other}\" at version {} \
                     could not be read to check for conflicts: {error}",
                    ours.name, entry.version
                ))
            })?;
            vacant.insert(deployment);
        }
    }
    let mut all: Vec<_> = named()
        .filter_map(|(n, entry)| others.get(&(n.clone(), entry.version.clone())))
        .collect();
    all.push(ours);
    compose::check_deployments(&all)
}

/// Refuses a bucket that ignores conditional writes, on an object no other deploy writes: a `.`
/// is in no deployment's name.
async fn check_conditional_writes(bucket: &Bucket) -> Result<()> {
    let mut nonce = [0; 16];
    SecureRandom::fill(&SystemRandom::new(), &mut nonce)
        .map_err(|_| Error::Deploy("could not generate random numbers".into()))?;
    let probe = format!("deploy/{}.probe", hex(&nonce));
    crate::cluster::check_conditional_writes(bucket, &probe).await
}

/// Resolves a catalog write that failed with `error` but may have succeeded.
///
/// Returns a successful publish when the catalog names `version` of `name`. Returns `None` when
/// the catalog is still `base`, so the write can be retried. Otherwise the outcome remains unknown
/// because writing again could undo a later deploy.
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
            "is at revision {} and does not name this version",
            now.revision
        ),
        None => "is missing".to_owned(),
    };
    Err(Error::Deploy(format!(
        "deployment \"{name}\" version {version} may have been published. Workerd could not \
         confirm the result because {error}. {CATALOG} {state}."
    )))
}

fn check_format(key: &str, format: u32) -> Result<()> {
    if format == FORMAT {
        Ok(())
    } else {
        Err(Error::Deploy(format!(
            "{key} uses format {format}. This workerd supports format {FORMAT}."
        )))
    }
}

#[cfg(test)]
#[path = "deploy-test.rs"]
mod tests;
