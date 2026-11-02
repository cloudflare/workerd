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
//!   refuses a manifest that lists any. Metadata about the version, such as who deployed it or
//!   its source revision, belongs here.
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
//! or edit `catalog.json` by hand to name it and raise `revision`. A node refuses a catalog or a
//! manifest whose format it does not read. It reads the catalog and its deployments once, at
//! startup.
//!
//! A deployment runs with the authority of each node's config. Its services can reach the node's
//! network, container engine and named services. Whoever can write `deploy/` has that authority
//! on every node, even when an IAM policy limits the writer to `v1/deploy/`.

use std::collections::BTreeMap;

use serde::Deserialize;
use serde::Serialize;

use crate::Error;
use crate::Result;
use crate::hex;
use crate::sha256;

/// The format of the catalog and the manifests this build reads and writes.
pub const FORMAT: u32 = 1;

pub const CATALOG: &str = "deploy/catalog.json";

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

/// One version of a deployment: `config` is its `config.bin`.
#[derive(Clone, Debug)]
pub struct Deployment {
    pub name: String,
    pub version: String,
    pub config: Vec<u8>,
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
            "\"{name}\" is not a valid deployment name. Use 1 to 64 ASCII letters, digits, \
             hyphens, or underscores. Start with a letter or digit."
        )))
    }
}

#[cfg(test)]
#[path = "deploy-test.rs"]
mod tests;
