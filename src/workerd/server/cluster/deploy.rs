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
//!
//! A name is 1 to 64 ASCII letters, digits, `-` and `_`, starting with a letter or digit. A
//! version's objects are never rewritten or removed when the catalog moves on, so a rollback is
//! a deploy of an earlier version, or, by hand, an edit of `catalog.json` that names it and
//! raises `revision`. A node refuses a catalog or a manifest whose format it does not read, and
//! reads the catalog and the deployments it names once, at startup.
//!
//! A deployment runs with the authority of each node's config: its services reach the node's
//! network, environment variables, container engine and named services. Whoever may write
//! `deploy/` has that authority on every node; limiting a writer to `deploy/` (with an IAM policy
//! on `v1/deploy/`, say) does not limit it.

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
            "\"{name}\" is not a deployment name: 1 to 64 letters, digits, '-' and '_', \
             starting with a letter or digit"
        )))
    }
}

#[cfg(test)]
#[path = "deploy-test.rs"]
mod tests;
