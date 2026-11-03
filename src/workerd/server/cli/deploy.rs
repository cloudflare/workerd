//! `workerd deploy`: publishes a deployment to a cluster's bucket (`cluster::deploy`), and with
//! `--wait` waits for the cluster's instances to serve it.
//!
//! The deployment is a wrangler project, translated by `wrangler.rs`, or a Cap'n Proto config.
//! The bucket is named on the command line, as `--bucket`, `--endpoint` and `--region`, and
//! opened with the client the instances use, whose credentials and other settings not given
//! fall back to the same environment variables (`cluster::open_bucket`). Nothing here reads an
//! instance's config: what can be checked without one is checked before anything is uploaded
//! ([`check`]), and against the catalog's other deployments as the catalog is written
//! (`deploy::publish`). Each instance checks the new catalog revision against its own config,
//! refuses one it cannot serve, and adopts one it can by restarting (`workerd_server::rollout`);
//! their records in the bucket name the revision each serves and the latest it refused, which
//! `--wait` reads.

use std::fmt::Display;
use std::path::Path;
use std::path::PathBuf;
use std::time::Duration;

use cluster::Bucket;
use cluster::compose;
use cluster::deploy;
use cluster::deploy::Published;
use cluster::rollout;
use tokio::runtime::Runtime;

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
            "{path}: wrangler.toml is not supported; convert it to wrangler.jsonc"
        ));
    }
    if wrangler::is_wrangler_config(path) {
        return Ok(Project::Wrangler(given.to_owned()));
    }
    Ok(Project::Capnp(path.to_owned()))
}

/// Checks what can be checked of `deployment`, an encoded `Config`, as deployment `name`
/// without the bucket or an instance's config: the name, that it holds only services, and its
/// size. Errors are a line each, as are those of [`Cluster`]'s methods.
pub fn check(name: &str, deployment: &[u8]) -> Result<(), Vec<String>> {
    deploy::check_name(name).map_err(|e| lines(&e))?;
    let canonical = compose::deployment_config(name, deployment).map_err(|e| lines(&e))?;
    if canonical.len() > deploy::MAX_CONFIG {
        return Err(vec![format!(
            "deployment \"{name}\" is {} bytes; the most a deployment can be is {}",
            canonical.len(),
            deploy::MAX_CONFIG
        )]);
    }
    Ok(())
}

/// How often `--wait` reads the instances' records.
const WAIT_POLL: Duration = Duration::from_secs(1);

/// The bucket of a cluster, and the runtime its client runs on.
pub struct Cluster {
    runtime: Runtime,
    bucket: Bucket,
}

impl Cluster {
    /// The bucket `url`, with `options` as for `cluster.bucketOptions`. Errors are a line each, as
    /// are those of the other methods.
    pub fn open(url: &str, options: &[String]) -> Result<Self, Vec<String>> {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .map_err(|e| lines(&e))?;
        let bucket = {
            let _runtime = runtime.enter();
            cluster::open_bucket(url, options).map_err(|e| lines(&format!("{url}: {e}")))?
        };
        Ok(Self { runtime, bucket })
    }

    /// Publishes `deployment`, an encoded `Config`, as deployment `name`. Answers what the catalog
    /// now holds.
    pub fn publish(&self, name: &str, deployment: &[u8]) -> Result<Published, Vec<String>> {
        self.runtime
            .block_on(deploy::publish(&self.bucket, name, deployment))
            .map_err(|e| lines(&e))
    }

    /// How many live instances serve now: as many must serve once a roll is done.
    pub fn serving(&self) -> Result<usize, Vec<String>> {
        let records = self
            .runtime
            .block_on(rollout::live_nodes(&self.bucket, &|| self.now_ms()))
            .map_err(|e| lines(&e))?;
        Ok(records.iter().filter(|r| r.revision.is_some()).count())
    }

    /// Waits until the roll to `published`, version `version` of deployment `name`, is done, for
    /// at most `timeout` (`rollout::Wait`, `baseline` the instances that served before).
    /// Progress goes to stderr.
    pub fn wait(
        &self,
        name: &str,
        published: &Published,
        baseline: usize,
        timeout: Duration,
    ) -> Result<(), String> {
        // The latest state: what progress says, or what failed to read.
        let mut said = "nothing read yet".to_owned();
        // The timer is made in the runtime, which it needs.
        let waited = self.runtime.block_on(async {
            tokio::time::timeout(timeout, self.poll(name, published, baseline, &mut said)).await
        });
        waited.unwrap_or_else(|_| Err(format!("timed out after {}s: {said}", timeout.as_secs())))
    }

    /// `wait`, with no timeout: `said` is the latest state.
    async fn poll(
        &self,
        name: &str,
        published: &Published,
        baseline: usize,
        said: &mut String,
    ) -> Result<(), String> {
        let mut wait = rollout::Wait::new(published.revision, baseline);
        // The latest error of each read, said once while it fails so.
        let mut failing: [Option<String>; 3] = Default::default();
        let mut report = |read: usize, what: &str, result: Result<(), String>| match result {
            Ok(()) => failing[read] = None,
            Err(e) if failing[read].as_ref() != Some(&e) => {
                eprintln!("deploy: reading {what} failed: {e}");
                failing[read] = Some(e);
            }
            Err(_) => {}
        };
        let mut first = true;
        loop {
            if !first {
                tokio::time::sleep(WAIT_POLL).await;
            }
            first = false;
            let catalog = deploy::catalog(&self.bucket).await;
            report(
                0,
                deploy::CATALOG,
                catalog.as_ref().map(drop).map_err(ToString::to_string),
            );
            let catalog = match catalog {
                Err(_) => rollout::CatalogSeen::Unreadable,
                Ok(None) => rollout::CatalogSeen::Absent,
                Ok(Some((catalog, _))) => rollout::CatalogSeen::Revision {
                    revision: catalog.revision,
                    names_version: catalog
                        .deployments
                        .get(name)
                        .is_some_and(|c| c.version == published.version),
                },
            };
            let roll = rollout::roll(&self.bucket).await;
            report(
                1,
                rollout::ROLL,
                roll.as_ref().map(drop).map_err(ToString::to_string),
            );
            let records = rollout::live_nodes(&self.bucket, &|| self.now_ms()).await;
            let what = "the nodes' records";
            report(
                2,
                what,
                records.as_ref().map(drop).map_err(ToString::to_string),
            );
            let roll = match &roll {
                Err(_) => rollout::RollSeen::Unreadable,
                Ok(None) => rollout::RollSeen::Absent,
                Ok(Some(roll)) => rollout::RollSeen::Present(roll),
            };
            match rollout::step(&mut wait, catalog, roll, records.as_deref().ok()) {
                rollout::Waited::Done(line) => {
                    eprintln!("deploy: {line}");
                    return Ok(());
                }
                rollout::Waited::Fail(reason) => return Err(reason),
                rollout::Waited::Waiting(line) => {
                    if line != *said {
                        eprintln!("deploy: {line}");
                        *said = line;
                    }
                }
            }
        }
    }

    /// The bucket's clock, which the instances' leases keep to.
    fn now_ms(&self) -> u64 {
        let skew = self.bucket.clock_skew_ms().unwrap_or(0);
        cluster::now_ms().saturating_add_signed(-skew)
    }
}

fn lines(e: &dyn Display) -> Vec<String> {
    e.to_string().lines().map(str::to_owned).collect()
}

#[cfg(test)]
#[path = "deploy-test.rs"]
mod tests;
