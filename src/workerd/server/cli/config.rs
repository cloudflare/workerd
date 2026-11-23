//! Where the config comes from: a Cap'n Proto schema file (compiled by C++, the only compiler for
//! that format), an encoded config file (`--binary`), stdin, the config compiled into the
//! executable. Every source is checked to be a well-formed `Config` message here, and ends up as
//! a [`Config`] for the C++ driver.

use std::fmt;
use std::fs::File;
use std::io;
use std::io::Read;
use std::io::Write;

use capnp::message::ReaderOptions;
use workerd_capnp::config;
use workerd_capnp::service;

use crate::bridge::ffi;
use crate::wrangler::CLUSTER_STORAGE;

/// A `Config` message, checked: encoded as a Cap'n Proto stream (segment table, then segments),
/// in 8-byte words.
pub struct Config(Vec<u64>);

impl Config {
    /// Checks that `bytes` are one well-formed message whose root reads as a `Config`, and keeps
    /// it re-encoded.
    pub fn from_message_bytes(mut bytes: &[u8]) -> capnp::Result<Self> {
        let message = capnp::serialize::read_message(&mut bytes, reader_options())?;
        // total_size() visits every pointer, so a malformed message fails here rather than in the
        // server.
        message.get_root::<config::Reader>()?.total_size()?;
        Ok(Self(words_from_bytes(
            &capnp::serialize::write_message_segments_to_words(&message.into_segments()),
        )))
    }

    /// The same, for a message already in words (the config compiler's output, or the config
    /// compiled into the executable).
    pub fn from_words(words: &[u64]) -> capnp::Result<Self> {
        Self::from_message_bytes(&bytes_from_words(words))
    }

    /// Reads an encoded config file (`--binary <config-file>`).
    pub fn read_file(path: &str) -> Result<Self, String> {
        let mut file = File::open(path).map_err(|_| "No such file.".to_owned())?;
        // Check that we have a file, not a directory.
        if !file
            .metadata()
            .map_err(|error| error.to_string())?
            .is_file()
        {
            return Err("Config path is not a file.".to_owned());
        }
        let mut bytes = Vec::new();
        file.read_to_end(&mut bytes)
            .map_err(|error| error.to_string())?;
        Self::from_message_bytes(&bytes).map_err(|error| error.to_string())
    }

    /// Reads one encoded config message from stdin (`--binary -`). Unbuffered, so nothing after
    /// the message is consumed: the config may tell the server to read stdin itself.
    pub fn read_stdin() -> Result<Self, String> {
        let mut stdin = stdin_file().map_err(|error| error.to_string())?;
        let message = capnp::serialize::read_message(&mut stdin, reader_options())
            .map_err(|error| error.to_string())?;
        message
            .get_root::<config::Reader>()
            .and_then(|root| root.total_size())
            .map_err(|error| error.to_string())?;
        Ok(Self(words_from_bytes(
            &capnp::serialize::write_message_segments_to_words(&message.into_segments()),
        )))
    }

    pub fn into_words(self) -> Vec<u64> {
        self.0
    }

    /// The encoded config as bytes.
    pub fn to_bytes(&self) -> Vec<u8> {
        bytes_from_words(&self.0)
    }

    /// The config as `serve` runs it in a cluster, decoded and encoded once.
    ///
    /// First, a config without `cluster` joins one at `flags.address`, the value of
    /// `--cluster-addr`, if it defines [`CLUSTER_STORAGE`] as a writable disk service: it gets a
    /// `cluster` with that address and every other setting at its default, so the key and bucket
    /// come from the environment and an `s3://` bucket's endpoint and credentials from the
    /// `AWS_*` variables. A config with a `cluster` keeps it, since
    /// `--cluster-addr` overrides its address. A config without either is unchanged, and then
    /// `flags.advertise`, the value of `--cluster-advertise`, is an error.
    ///
    /// Then, with `var`, the cluster's `key` and `bucket` are filled from the environment where
    /// they are empty. See `ClusterConfig` in workerd.capnp. `var` reads an environment variable
    /// and returns `None` when it is unset or empty. A key or bucket that neither the config nor
    /// the environment sets is an error.
    pub fn with_cluster(
        self,
        flags: &ClusterFlags<'_>,
        var: Option<Env<'_>>,
    ) -> Result<Self, ClusterError> {
        let usage = |error: capnp::Error| ClusterError::Usage(error.to_string());
        let bytes = self.to_bytes();
        let message =
            capnp::serialize::read_message(bytes.as_slice(), reader_options()).map_err(usage)?;
        let root = message.get_root::<config::Reader>().map_err(usage)?;
        let join = if root.has_cluster() {
            None
        } else {
            match (flags.address, flags.advertise) {
                (Some(_), _) if !flags.can_join => {
                    return Err(ClusterError::Usage(
                        "--cluster-addr: the config compiled into this executable has no \
                         cluster, so it cannot join one. Serve the config with `workerd serve \
                         --cluster-addr` instead, or compile a config that has `cluster`."
                            .to_owned(),
                    ));
                }
                (Some(_), _) if !has_cluster_storage(root).map_err(usage)? => {
                    return Err(ClusterError::Usage(format!(
                        "--cluster-addr: the config has no writable disk service named \
                         `{CLUSTER_STORAGE}`, which holds the Durable Objects of the cluster's \
                         deployments on each instance, so it cannot join a cluster. Add one, or \
                         serve the wrangler config itself, which defines it when joined."
                    )));
                }
                (Some(address), _) => Some(address),
                (None, Some(_)) => {
                    return Err(ClusterError::Usage(
                        "--cluster-advertise: the config has no cluster. Pass --cluster-addr to \
                         join one."
                            .to_owned(),
                    ));
                }
                (None, None) => return Ok(self),
            }
        };
        let (key, bucket) = match var {
            None => (None, None),
            Some(var) => {
                // A joined config's cluster is new, so its key and bucket are empty.
                let (key_empty, bucket_empty) = if join.is_some() {
                    (true, true)
                } else {
                    let cluster = root.get_cluster().map_err(usage)?;
                    let empty = |text: capnp::Result<capnp::text::Reader<'_>>| {
                        text.and_then(|text| Ok(text.to_str()?.is_empty()))
                            .map_err(usage)
                    };
                    (empty(cluster.get_key())?, empty(cluster.get_bucket())?)
                };
                cluster_environment(var, key_empty, bucket_empty)
                    .map_err(ClusterError::Environment)?
            }
        };
        if join.is_none() && key.is_none() && bucket.is_none() {
            return Ok(self);
        }
        let mut message = capnp::message::Builder::new_default();
        message.set_root(root).map_err(usage)?;
        let root = message.get_root::<config::Builder>().map_err(usage)?;
        let mut cluster = match join {
            Some(address) => {
                let mut cluster = root.init_cluster();
                cluster.set_address(address);
                cluster
            }
            None => root.get_cluster().map_err(usage)?,
        };
        if let Some(key) = key {
            cluster.set_key(key.as_str());
        }
        if let Some(bucket) = bucket {
            cluster.set_bucket(bucket.as_str());
        }
        Ok(Self(words_from_bytes(
            &capnp::serialize::write_message_to_words(&message),
        )))
    }

    /// Writes the encoded config to stdout (`compile --config-only`).
    pub fn write_to_stdout(&self) -> io::Result<()> {
        let mut out = io::stdout().lock();
        for word in &self.0 {
            out.write_all(&word.to_ne_bytes())?;
        }
        out.flush()
    }
}

/// Reads an environment variable: `None` when it is unset or empty.
pub type Env<'a> = &'a dyn Fn(&str) -> Option<String>;

/// What `serve` was told about the cluster: `--cluster-addr` and `--cluster-advertise`, and
/// whether a config without `cluster` may join one.
#[derive(Default)]
pub struct ClusterFlags<'a> {
    pub address: Option<&'a str>,
    pub advertise: Option<&'a str>,
    /// False for the config compiled into an executable: `compile` translates a wrangler config
    /// for a server that joins no cluster, so the binary has no way to become a node of one.
    pub can_join: bool,
}

/// Why [`Config::with_cluster`] refused a config.
#[derive(Debug)]
pub enum ClusterError {
    /// The command line does not fit the config.
    Usage(String),
    /// A key or bucket is missing, or the environment variable for one is unusable.
    Environment(String),
}

impl fmt::Display for ClusterError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Usage(error) | Self::Environment(error) => f.write_str(error),
        }
    }
}

/// Whether `root` defines [`CLUSTER_STORAGE`] as a writable disk service, which a config that
/// joins a cluster needs for the Durable Objects of the cluster's deployments.
fn has_cluster_storage(root: config::Reader<'_>) -> capnp::Result<bool> {
    for service in root.get_services()? {
        if service.get_name()?.to_str()? == CLUSTER_STORAGE {
            return Ok(match service.which()? {
                service::Which::Disk(disk) => disk?.get_writable(),
                _ => false,
            });
        }
    }
    Ok(false)
}

/// The key and bucket to fill from the environment, for a cluster whose key and bucket are empty
/// as `key_empty` and `bucket_empty` say. `None` for one the config sets. A key or bucket that
/// neither sets is an error.
fn cluster_environment(
    var: Env<'_>,
    key_empty: bool,
    bucket_empty: bool,
) -> Result<(Option<String>, Option<String>), String> {
    let key = if key_empty { cluster_key(var)? } else { None };
    let bucket = if bucket_empty {
        var(CLUSTER_BUCKET).and_then(|bucket| non_blank(&bucket))
    } else {
        None
    };
    let mut missing = Vec::new();
    if key_empty && key.is_none() {
        missing.push(format!(
            "cluster.key is not set. Set it in the config, in {CLUSTER_KEY}, or in a file named by \
             {CLUSTER_KEY_FILE}."
        ));
    }
    if bucket_empty && bucket.is_none() {
        missing.push(format!(
            "cluster.bucket is not set. Set it in the config or in {CLUSTER_BUCKET}."
        ));
    }
    if missing.is_empty() {
        Ok((key, bucket))
    } else {
        Err(missing.join("\n"))
    }
}

/// The environment variable that holds the cluster key when the config leaves it empty.
pub const CLUSTER_KEY: &str = "WORKERD_CLUSTER_KEY";
/// The environment variable that names a file holding the cluster key, such as a mounted secret.
pub const CLUSTER_KEY_FILE: &str = "WORKERD_CLUSTER_KEY_FILE";
/// The environment variable that holds the cluster bucket when the config leaves it empty.
pub const CLUSTER_BUCKET: &str = "WORKERD_CLUSTER_BUCKET";

/// The shortest cluster key. The server refuses a shorter one in the config; one from the
/// environment is refused here, where the error can name its source.
const MIN_KEY: usize = 32;

/// The cluster key from `CLUSTER_KEY` or the file `CLUSTER_KEY_FILE` names, without trailing
/// whitespace, such as a file's final newline. `None` when neither is set or the key is blank.
fn cluster_key(var: Env<'_>) -> Result<Option<String>, String> {
    let (key, source) = match (var(CLUSTER_KEY), var(CLUSTER_KEY_FILE)) {
        (Some(_), Some(_)) => {
            return Err(format!(
                "cluster: set only one of {CLUSTER_KEY} and {CLUSTER_KEY_FILE}."
            ));
        }
        (Some(key), None) => (non_blank(&key), CLUSTER_KEY.to_owned()),
        (None, Some(path)) => {
            let source = format!("{CLUSTER_KEY_FILE}={path}");
            let key =
                read_key_file(&path).map_err(|error| format!("cluster: {source}: {error}"))?;
            (non_blank(&key), source)
        }
        (None, None) => return Ok(None),
    };
    match key {
        Some(key) if key.len() < MIN_KEY => Err(format!(
            "cluster: {source}: the key is {} bytes, and must be at least {MIN_KEY}.",
            key.len()
        )),
        key => Ok(key),
    }
}

/// `value` without trailing whitespace, or `None` if nothing is left.
fn non_blank(value: &str) -> Option<String> {
    Some(value.trim_end())
        .filter(|value| !value.is_empty())
        .map(str::to_owned)
}

/// The largest key file read. A key is a few dozen bytes; the limit keeps a mistaken path, such
/// as a device, from being read without end.
const MAX_KEY_FILE: u64 = 4096;

/// The text in the regular file at `path`.
fn read_key_file(path: &str) -> Result<String, String> {
    // Checked before opening: opening a FIFO blocks, and a device may never end. The check
    // follows symlinks, as Kubernetes mounts a Secret's files through them.
    if !std::fs::metadata(path)
        .map_err(|error| error.to_string())?
        .is_file()
    {
        return Err("not a regular file".to_owned());
    }
    let file = File::open(path).map_err(|error| error.to_string())?;
    let mut key = Vec::new();
    file.take(MAX_KEY_FILE + 1)
        .read_to_end(&mut key)
        .map_err(|error| error.to_string())?;
    if key.len() as u64 > MAX_KEY_FILE {
        return Err(format!("larger than {MAX_KEY_FILE} bytes"));
    }
    String::from_utf8(key).map_err(|_| "not UTF-8 text. The key must be text.".to_owned())
}

/// Configs can legitimately be very large and are not malicious, so use an effectively-infinite
/// traversal limit.
fn reader_options() -> ReaderOptions {
    *ReaderOptions::new().traversal_limit_in_words(None)
}

impl fmt::Display for ffi::ConfigParseError {
    /// `file:line:col-endcol: message`, the way compilers print it.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}:{}", self.file, self.line, self.column)?;
        if self.end_column != 0 {
            write!(f, "-{}", self.end_column)?;
        }
        write!(f, ": {}", self.message)
    }
}

/// Converts bytes to native-endian words, ignoring a trailing partial word.
pub fn words_from_bytes(bytes: &[u8]) -> Vec<u64> {
    bytes
        .chunks_exact(8)
        .map(|chunk| {
            let mut word = [0u8; 8];
            word.copy_from_slice(chunk);
            u64::from_ne_bytes(word)
        })
        .collect()
}

fn bytes_from_words(words: &[u64]) -> Vec<u8> {
    words.iter().flat_map(|word| word.to_ne_bytes()).collect()
}

/// An unbuffered handle to stdin, sharing the process's descriptor.
#[cfg(unix)]
fn stdin_file() -> io::Result<File> {
    use std::os::fd::AsFd;
    Ok(File::from(io::stdin().as_fd().try_clone_to_owned()?))
}

#[cfg(windows)]
fn stdin_file() -> io::Result<File> {
    use std::os::windows::io::AsHandle;
    Ok(File::from(io::stdin().as_handle().try_clone_to_owned()?))
}

/// An unbuffered handle to stdout, so a whole executable can be copied into it efficiently.
#[cfg(unix)]
pub fn stdout_file() -> io::Result<File> {
    use std::os::fd::AsFd;
    Ok(File::from(io::stdout().as_fd().try_clone_to_owned()?))
}

#[cfg(windows)]
pub fn stdout_file() -> io::Result<File> {
    use std::os::windows::io::AsHandle;
    Ok(File::from(io::stdout().as_handle().try_clone_to_owned()?))
}

#[cfg(test)]
#[path = "config-test.rs"]
mod tests;
