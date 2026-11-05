//! Loads the cluster deployments that a node serves beside its own config.
//!
//! `serve` reads the bucket's catalog once before the server starts. It composes deployments in
//! catalog order and passes the server the composed config, each service's source deployment, and
//! the serving or skipping lines to log. The server attributes a deployment's service errors to
//! that deployment, so they do not stop the node. A deployment that cannot be read or does not
//! compose is skipped as a whole. Only a failure to open the bucket or reach or read its catalog
//! ends the process.
//!
//! Without `--experimental`, without `Config.cluster`, or with no catalog in the bucket, the
//! config is left exactly as it is.

use capnp::message::ReaderOptions;
use workerd_capnp::config;
use workerd_server::config::DeploymentLog;
use workerd_server::config::Deployments;

use crate::config::Config;

/// Why the deployments could not be added, each error a line to print.
#[derive(Debug)]
pub enum Failure {
    /// The config, or a deployment against it: fixing the config may fix it.
    Config(Vec<String>),
    /// Opening the bucket or reading its catalog. Opening can fail on the environment (credentials)
    /// as well as on the config, so these end the process rather than wait for a change of the
    /// config files.
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
) -> Result<(Config, Deployments), Failure> {
    let bytes = config.to_bytes();
    let (url, options) = match cluster_bucket(&bytes, address, advertise) {
        Ok(Some(bucket)) => bucket,
        Ok(None) => return Ok((config, Deployments::default())),
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
    // A catalog load failure ends the process, and its supervisor restarts it.
    // TODO(someday): Retry transient catalog failures here before exiting.
    let loaded = runtime
        .block_on(cluster::deploy::load(&bucket))
        .map_err(|e| Failure::Remote(lines(&e)))?;
    // In a cluster, the node's references to services it does not define are treated as deployment
    // references, even before the first deploy.
    let mut deployments = Deployments {
        enabled: true,
        ..Deployments::default()
    };
    let Some(loaded) = loaded else {
        return Ok((config, deployments));
    };
    let composed = cluster::compose::merge(&bytes, &loaded.deployments).map_err(|e| {
        let message = e.to_string();
        Failure::Config(
            message
                .lines()
                .map(|line| format!("cluster: {line}"))
                .collect(),
        )
    })?;
    deployments.log.push(DeploymentLog::Info(format!(
        "cluster: serving deployments from catalog revision {}",
        loaded.revision
    )));
    // TODO(soon): Keep on disk the last catalog revision whose deployments all composed and built
    //   successfully on this node. Use that revision's version of a deployment when the current
    //   revision skips it or the server finds it broken.
    // Sort readable and unreadable deployments into catalog order, which is the order of names.
    let mut compositions = composed.deployments;
    deployments.unreadable.extend(
        loaded
            .unreadable
            .iter()
            .map(|unreadable| unreadable.name.clone()),
    );
    compositions.extend(loaded.unreadable.into_iter().map(|unreadable| {
        cluster::compose::Composition {
            name: unreadable.name,
            version: unreadable.version,
            services: Vec::new(),
            errors: vec![unreadable.error],
        }
    }));
    compositions.sort_by(|a, b| a.name.cmp(&b.name));
    for composition in compositions {
        let cluster::compose::Composition {
            name,
            version,
            services,
            errors,
        } = composition;
        if errors.is_empty() {
            deployments.log.push(DeploymentLog::Info(format!(
                "cluster: serving deployment \"{name}\" at version {version}"
            )));
            for service in services {
                deployments.services.insert(service, name.clone());
            }
        } else {
            // Each error names the deployment.
            for error in errors {
                deployments.log.push(DeploymentLog::Error(format!(
                    "cluster: skipping a deployment at version {version}: {error}"
                )));
            }
            for service in services {
                deployments
                    .skipped
                    .entry(service)
                    .or_insert_with(|| name.clone());
            }
        }
    }
    let config =
        Config::from_message_bytes(&composed.config).map_err(|e| Failure::Config(lines(&e)))?;
    Ok((config, deployments))
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
