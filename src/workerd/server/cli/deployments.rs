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

/// Merges the deployments from `config`'s cluster bucket into `config` after checking the cluster's
/// addresses.
///
/// `address` and `advertise` are the values of `--cluster-addr` and `--cluster-advertise`.
pub fn add(
    config: Config,
    address: Option<&str>,
    advertise: Option<&str>,
) -> Result<Config, Failure> {
    let bytes = config.to_bytes();
    let (url, options) = match cluster_bucket(&bytes, address, advertise) {
        Ok(Some(bucket)) => bucket,
        Ok(None) => return Ok(config),
        Err(errors) => return Err(Failure::Config(errors)),
    };
    let lines = |e: &dyn std::fmt::Display| {
        let message = format!("cluster: could not load deployments from {url}: {e}");
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
    // A load failure ends the process, and its supervisor restarts it.
    // TODO(someday): Retry transient bucket failures here before exiting.
    let loaded = runtime
        .block_on(cluster::deploy::load(&bucket))
        .map_err(|e| Failure::Remote(lines(&e)))?;
    let Some(loaded) = loaded else {
        return Ok(config);
    };
    let merged = cluster::compose::merge(&bytes, &loaded.deployments).map_err(|e| {
        let message = e.to_string();
        Failure::Config(
            message
                .lines()
                .map(|line| format!("cluster: {line}"))
                .collect(),
        )
    })?;
    eprintln!(
        "cluster: serving deployments from catalog revision {}",
        loaded.revision
    );
    for deployment in &loaded.deployments {
        eprintln!(
            "cluster: serving deployment \"{}\" at version {}",
            deployment.name, deployment.version
        );
    }
    Config::from_message_bytes(&merged).map_err(|e| Failure::Config(lines(&e)))
}

/// Returns the config's cluster bucket URL and options after checking its addresses.
///
/// Returns `None` if the config has no cluster, or errors ready to print if validation fails.
fn cluster_bucket(
    bytes: &[u8],
    address: Option<&str>,
    advertise: Option<&str>,
) -> Result<Option<(String, Vec<String>)>, Vec<String>> {
    let config_error = |e: capnp::Error| vec![format!("cluster: {e}")];
    let mut bytes = bytes;
    let options = *ReaderOptions::new().traversal_limit_in_words(None);
    let message = capnp::serialize::read_message(&mut bytes, options).map_err(config_error)?;
    let root = message
        .get_root::<config::Reader<'_>>()
        .map_err(config_error)?;
    if !root.has_cluster() {
        return Ok(None);
    }
    let cluster = root.get_cluster().map_err(config_error)?;
    workerd_server::cluster::check_addresses(cluster, address, advertise).map_err(|e| {
        e.description()
            .lines()
            .map(str::to_owned)
            .collect::<Vec<_>>()
    })?;
    let url = text(cluster.get_bucket()).map_err(config_error)?;
    let options = cluster
        .get_bucket_options()
        .and_then(|options| options.iter().map(text).collect())
        .map_err(config_error)?;
    Ok(Some((url, options)))
}

fn text(text: capnp::Result<capnp::text::Reader<'_>>) -> capnp::Result<String> {
    Ok(text?.to_string()?)
}

#[cfg(test)]
#[path = "deployments-test.rs"]
mod tests;
