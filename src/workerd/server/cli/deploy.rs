//! Implements `workerd deploy` for a cluster bucket.
//!
//! A deployment is a wrangler project translated by `wrangler.rs` or a Cap'n Proto config. The
//! command-line options `--bucket`, `--endpoint`, and `--region` name the bucket. The command opens
//! it with the client that instances use. Missing credentials and settings use the same environment
//! variables (`cluster::open_bucket`).
//!
//! The command does not read an instance's config. [`check`] validates the deployment before any
//! upload. `deploy::publish` checks it against the catalog's other deployments before uploading it
//! and again when writing the catalog. At startup, each instance checks deployments in catalog
//! order against its own config and accepted deployments. It skips a deployment as a whole and
//! logs why when the configs do not compose (`deployments.rs`).

use std::fmt::Display;
use std::path::Path;
use std::path::PathBuf;

use cluster::compose;
use cluster::deploy;
use cluster::deploy::Published;

use crate::wrangler;

/// Where a deployment comes from: `<project>` as `workerd deploy` reads it.
#[derive(Debug, PartialEq, Eq)]
pub enum Project {
    /// A wrangler config, found in the project's directory or named.
    Wrangler(PathBuf),
    /// A Cap'n Proto config: a schema file, or with `--binary` an encoded message.
    Capnp(String),
}

/// Reads `<project>`: a directory holds a wrangler config, as does a `.json` or `.jsonc` file;
/// any other file is a Cap'n Proto config, as it is for `serve`, and so is anything given with
/// `--binary`, `-` for stdin included.
pub fn project(path: &str, binary: bool) -> Result<Project, String> {
    if binary {
        return Ok(Project::Capnp(path.to_owned()));
    }
    let given = Path::new(path);
    if !given.exists() {
        return Err(format!("{path}: No such file or directory."));
    }
    if given.is_dir() {
        return wrangler::project_config(given).map(Project::Wrangler);
    }
    let extension = given.extension().unwrap_or_default();
    if extension.eq_ignore_ascii_case("toml") {
        return Err(format!(
            "{path}: wrangler.toml is not supported. Convert it to wrangler.jsonc."
        ));
    }
    if wrangler::is_wrangler_config(path) {
        return Ok(Project::Wrangler(given.to_owned()));
    }
    Ok(Project::Capnp(path.to_owned()))
}

/// Checks what can be checked of `deployment`, an encoded `Config`, as deployment `name`
/// without the bucket or an instance's config: the name, that it holds only services, and its
/// size. Errors are a line each, as are those of [`publish`].
pub fn check(name: &str, deployment: &[u8]) -> Result<(), Vec<String>> {
    deploy::check_name(name).map_err(|e| lines(&e))?;
    let canonical = compose::deployment_config(name, deployment).map_err(|e| lines(&e))?;
    if canonical.len() > deploy::MAX_CONFIG {
        return Err(vec![format!(
            "deployment \"{name}\" is {} bytes. The maximum size is {} bytes.",
            canonical.len(),
            deploy::MAX_CONFIG
        )]);
    }
    Ok(())
}

/// Publishes `deployment`, an encoded `Config`, as deployment `name` to the bucket `url`, with
/// `options` as for `cluster.bucketOptions`. Answers what the catalog now holds.
pub fn publish(
    url: &str,
    options: &[String],
    name: &str,
    deployment: &[u8],
) -> Result<Published, Vec<String>> {
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .map_err(|e| lines(&e))?;
    let bucket = {
        let _runtime = runtime.enter();
        cluster::open_bucket(url, options).map_err(|e| lines(&format!("{url}: {e}")))?
    };
    runtime
        .block_on(deploy::publish(&bucket, name, deployment))
        .map_err(|e| lines(&e))
}

fn lines(e: &dyn Display) -> Vec<String> {
    e.to_string().lines().map(str::to_owned).collect()
}

#[cfg(test)]
#[path = "deploy-test.rs"]
mod tests;
