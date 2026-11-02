//! Cluster mode's deployments (`cluster::deploy`): a node serves, beside its own config, the
//! deployments its bucket's catalog names. `serve` reads them once, before the server starts,
//! and hands the server one config, the node's with their services appended
//! (`cluster::compose`). Without `--experimental`, without `Config.cluster`, or with no catalog
//! in the bucket, the config is left exactly as it is.

use capnp::message::ReaderOptions;
use workerd_capnp::config;

use crate::config::Config;

/// Why the deployments could not be added, each error a line to print.
#[derive(Debug)]
pub enum Failure {
    /// The config, or a deployment against it: fixing the config may fix it.
    Config(Vec<String>),
    /// Opening or reading the bucket, or what it holds. Opening can fail on the environment
    /// (credentials) as well as on the config, so these end the process rather than wait for a
    /// change of the config files.
    Remote(Vec<String>),
}

/// `config` with the deployments of its cluster's bucket merged in.
pub fn add(config: Config) -> Result<Config, Failure> {
    let bytes = config.to_bytes();
    let (url, options) = match cluster_bucket(&bytes) {
        Ok(Some(bucket)) => bucket,
        Ok(None) => return Ok(config),
        Err(e) => return Err(Failure::Config(vec![format!("cluster: {e}")])),
    };
    let lines = |e: &dyn std::fmt::Display| {
        let message = format!("cluster: loading deployments from {url}: {e}");
        message.lines().map(str::to_owned).collect::<Vec<_>>()
    };
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .map_err(|e| Failure::Remote(lines(&e)))?;
    let bucket = {
        let _runtime = runtime.enter();
        cluster::open_bucket(&url, &options).map_err(|e| Failure::Remote(lines(&e)))?
    };
    let loaded = runtime
        .block_on(cluster::deploy::load(&bucket))
        .map_err(|e| Failure::Remote(lines(&e)))?;
    let Some(loaded) = loaded else {
        return Ok(config);
    };
    eprintln!("cluster: serving catalog revision {}", loaded.revision);
    for deployment in &loaded.deployments {
        eprintln!(
            "cluster: deployment \"{}\" version {}",
            deployment.name, deployment.version
        );
    }
    let is_set = |variable: &str| std::env::var_os(variable).is_some();
    let merged = cluster::compose::merge(&bytes, &loaded.deployments, &is_set).map_err(|e| {
        let message = e.to_string();
        Failure::Config(
            message
                .lines()
                .map(|line| format!("cluster: {line}"))
                .collect(),
        )
    })?;
    Config::from_message_bytes(&merged).map_err(|e| Failure::Config(lines(&e)))
}

/// The bucket URL and options of the config's cluster, if it has one.
fn cluster_bucket(bytes: &[u8]) -> capnp::Result<Option<(String, Vec<String>)>> {
    let mut bytes = bytes;
    let options = *ReaderOptions::new().traversal_limit_in_words(None);
    let message = capnp::serialize::read_message(&mut bytes, options)?;
    let root = message.get_root::<config::Reader<'_>>()?;
    if !root.has_cluster() {
        return Ok(None);
    }
    let cluster = root.get_cluster()?;
    let url = cluster.get_bucket()?.to_string()?;
    let options = cluster.get_bucket_options()?;
    let options = options
        .iter()
        .map(|option| Ok(option?.to_string()?))
        .collect::<capnp::Result<_>>()?;
    Ok(Some((url, options)))
}

#[cfg(test)]
#[path = "deployments-test.rs"]
mod tests;
