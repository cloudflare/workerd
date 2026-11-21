use super::*;

fn read(config: &Config) -> capnp::message::Reader<capnp::serialize::OwnedSegments> {
    capnp::serialize::read_message(bytes_from_words(&config.0).as_slice(), reader_options())
        .unwrap()
}

/// A config with one service named "main".
fn one_service_config() -> Config {
    let mut message = capnp::message::Builder::new_default();
    let config = message.init_root::<config::Builder>();
    config.init_services(1).get(0).set_name("main");
    Config(words_from_bytes(&capnp::serialize::write_message_to_words(
        &message,
    )))
}

#[test]
fn config_bytes_are_checked() {
    let good = one_service_config();
    let round_trip = Config::from_words(&good.0).unwrap();
    assert_eq!(round_trip.0, good.0);

    assert!(Config::from_message_bytes(b"not a capnp message!").is_err());
    // A truncated message fails too.
    let bytes = bytes_from_words(&good.0);
    assert!(Config::from_message_bytes(&bytes[..bytes.len() - 8]).is_err());
}

/// A config with a cluster whose key and bucket are `key` and `bucket`, or with no cluster.
fn cluster_config(cluster: Option<(&str, &str)>) -> Config {
    let mut message = capnp::message::Builder::new_default();
    let mut root = message.init_root::<config::Builder>();
    root.reborrow().init_services(1).get(0).set_name("main");
    if let Some((key, bucket)) = cluster {
        let mut cluster = root.init_cluster();
        cluster.set_key(key);
        cluster.set_bucket(bucket);
        cluster.set_address("127.0.0.1:0");
    }
    Config(words_from_bytes(&capnp::serialize::write_message_to_words(
        &message,
    )))
}

/// A config without a cluster that can join one: it defines the disk service `do-storage`.
fn joinable_config() -> Config {
    let mut message = capnp::message::Builder::new_default();
    let mut services = message.init_root::<config::Builder>().init_services(2);
    services.reborrow().get(0).set_name("main");
    let mut storage = services.get(1);
    storage.set_name(CLUSTER_STORAGE);
    let mut disk = storage.init_disk();
    disk.set_path("/var/lib/workerd/do");
    disk.set_writable(true);
    Config(words_from_bytes(&capnp::serialize::write_message_to_words(
        &message,
    )))
}

/// The cluster's key and bucket in `config`.
fn cluster_of(config: &Config) -> (String, String) {
    let message = read(config);
    let cluster = message
        .get_root::<config::Reader>()
        .unwrap()
        .get_cluster()
        .unwrap();
    let text = |t: capnp::text::Reader<'_>| t.to_string().unwrap();
    (
        text(cluster.get_key().unwrap()),
        text(cluster.get_bucket().unwrap()),
    )
}

/// [`Config::with_cluster`] in its two halves, as most tests take them.
trait Halves: Sized {
    /// Fills the key and bucket from `var`, joining no cluster.
    fn fill(self, var: impl Fn(&str) -> Option<String>) -> Result<Config, String>;
    /// Joins a cluster at `address` as `serve` does, filling nothing.
    fn join(self, address: Option<&str>, advertise: Option<&str>) -> Result<Config, String>;
}

impl Halves for Config {
    fn fill(self, var: impl Fn(&str) -> Option<String>) -> Result<Config, String> {
        let var: Env<'_> = &var;
        (self.with_cluster(&ClusterFlags::default(), Some(var))).map_err(|e| e.to_string())
    }

    fn join(self, address: Option<&str>, advertise: Option<&str>) -> Result<Config, String> {
        let flags = ClusterFlags {
            address,
            advertise,
            can_join: true,
        };
        self.with_cluster(&flags, None).map_err(|e| e.to_string())
    }
}

/// An environment of `vars`.
fn env<'a>(vars: &'a [(&'a str, &'a str)]) -> impl Fn(&str) -> Option<String> + 'a {
    move |name| {
        vars.iter()
            .find(|(var, _)| *var == name)
            .map(|(_, value)| (*value).to_owned())
    }
}

/// A file of its own in the temporary directory, holding `contents`, removed when dropped.
struct TempFile(std::path::PathBuf);

impl TempFile {
    fn new(name: &str, contents: impl AsRef<[u8]>) -> Self {
        let path = std::env::temp_dir().join(format!("{name}-{}", std::process::id()));
        std::fs::write(&path, contents).unwrap();
        Self(path)
    }

    fn path(&self) -> &str {
        self.0.to_str().unwrap()
    }
}

impl Drop for TempFile {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

const KEY: &str = "0123456789abcdef0123456789abcdef";

#[test]
fn cluster_environment_fills_an_empty_key_and_bucket() {
    let vars = [(CLUSTER_KEY, KEY), (CLUSTER_BUCKET, "s3://bucket/prefix")];
    let config = cluster_config(Some(("", ""))).fill(env(&vars)).unwrap();
    assert_eq!(
        cluster_of(&config),
        (KEY.to_owned(), "s3://bucket/prefix".to_owned())
    );
}

#[test]
fn cluster_environment_does_not_replace_config_values() {
    let vars = [
        (CLUSTER_KEY, "from-the-environment"),
        (CLUSTER_BUCKET, "s3://other/prefix"),
    ];
    let config = cluster_config(Some((KEY, "s3://bucket/prefix")));
    let before = config.0.clone();
    let config = config.fill(env(&vars)).unwrap();
    assert_eq!(config.0, before);
}

#[test]
fn cluster_environment_leaves_a_config_without_cluster_unchanged() {
    let vars = [(CLUSTER_KEY, KEY), (CLUSTER_BUCKET, "s3://bucket/prefix")];
    let config = cluster_config(None);
    let before = config.0.clone();
    let config = config.fill(env(&vars)).unwrap();
    assert_eq!(config.0, before);
}

#[test]
fn cluster_environment_reads_the_key_file_without_trailing_whitespace() {
    let file = TempFile::new("workerd-cluster-key-test", format!("{KEY}\n"));
    let vars = [(CLUSTER_KEY_FILE, file.path())];
    let config = cluster_config(Some(("", "s3://bucket/prefix")))
        .fill(env(&vars))
        .unwrap();
    assert_eq!(cluster_of(&config).0, KEY);
}

#[test]
fn cluster_environment_trims_the_key_variable() {
    let key = format!("{KEY} \n");
    let vars = [(CLUSTER_KEY, key.as_str())];
    let config = cluster_config(Some(("", "s3://bucket/prefix")))
        .fill(env(&vars))
        .unwrap();
    assert_eq!(cluster_of(&config).0, KEY);
}

#[test]
fn cluster_environment_trims_the_bucket_variable() {
    let vars = [(CLUSTER_BUCKET, "s3://bucket/prefix\n")];
    let config = cluster_config(Some((KEY, ""))).fill(env(&vars)).unwrap();
    assert_eq!(cluster_of(&config).1, "s3://bucket/prefix");
}

#[test]
fn cluster_environment_refuses_a_short_key_naming_its_source() {
    let file = TempFile::new("workerd-cluster-key-short-test", "0123456789ab\n");
    for (vars, expected) in [
        (
            vec![(CLUSTER_KEY, "0123456789ab")],
            "cluster: WORKERD_CLUSTER_KEY: the key is 12 bytes, and must be at least 32."
                .to_owned(),
        ),
        (
            vec![(CLUSTER_KEY_FILE, file.path())],
            format!(
                "cluster: WORKERD_CLUSTER_KEY_FILE={}: the key is 12 bytes, and must be at least \
                 32.",
                file.path()
            ),
        ),
    ] {
        let Err(error) = cluster_config(Some(("", "s3://bucket/prefix"))).fill(env(&vars)) else {
            panic!("expected an error for {vars:?}");
        };
        assert_eq!(error, expected);
    }
}

#[test]
fn cluster_environment_requires_a_key_and_bucket() {
    let missing_key = "cluster.key is not set. Set it in the config, in WORKERD_CLUSTER_KEY, or \
                       in a file named by WORKERD_CLUSTER_KEY_FILE.";
    let missing_bucket =
        "cluster.bucket is not set. Set it in the config or in WORKERD_CLUSTER_BUCKET.";
    let blank = TempFile::new("workerd-cluster-key-blank-test", "\n");
    for (cluster, vars, expected) in [
        (("", ""), vec![], format!("{missing_key}\n{missing_bucket}")),
        (
            ("", "s3://bucket/prefix"),
            vec![(CLUSTER_KEY, " \n")],
            missing_key.to_owned(),
        ),
        (
            ("", "s3://bucket/prefix"),
            vec![(CLUSTER_KEY_FILE, blank.path())],
            missing_key.to_owned(),
        ),
        ((KEY, ""), vec![], missing_bucket.to_owned()),
        (
            (KEY, ""),
            vec![(CLUSTER_BUCKET, " \n")],
            missing_bucket.to_owned(),
        ),
    ] {
        let Err(error) = cluster_config(Some(cluster)).fill(env(&vars)) else {
            panic!("expected an error for {cluster:?} and {vars:?}");
        };
        assert_eq!(error, expected);
    }
}

#[test]
fn cluster_environment_refuses_a_key_file_that_is_not_text() {
    let file = TempFile::new("workerd-cluster-key-binary-test", [0xff, 0xfe, 0x00, 0x80]);
    let vars = [(CLUSTER_KEY_FILE, file.path())];
    let Err(error) = cluster_config(Some(("", ""))).fill(env(&vars)) else {
        panic!("expected an error");
    };
    assert_eq!(
        error,
        format!(
            "cluster: WORKERD_CLUSTER_KEY_FILE={}: not UTF-8 text. The key must be text.",
            file.path()
        )
    );
}

#[test]
fn cluster_environment_refuses_both_key_variables() {
    let file = TempFile::new("workerd-cluster-key-both-test", KEY);
    let vars = [(CLUSTER_KEY, KEY), (CLUSTER_KEY_FILE, file.path())];
    let Err(error) = cluster_config(Some(("", ""))).fill(env(&vars)) else {
        panic!("expected an error");
    };
    assert_eq!(
        error,
        "cluster: set only one of WORKERD_CLUSTER_KEY and WORKERD_CLUSTER_KEY_FILE."
    );
}

#[test]
fn cluster_environment_refuses_a_key_file_that_is_not_a_regular_file_or_too_large() {
    let large = TempFile::new("workerd-cluster-key-large-test", "k".repeat(4097));
    let directory = std::env::temp_dir();
    for (path, reason) in [
        (directory.to_str().unwrap(), "not a regular file"),
        (large.path(), "larger than 4096 bytes"),
    ] {
        let vars = [(CLUSTER_KEY_FILE, path)];
        let Err(error) = cluster_config(Some(("", ""))).fill(env(&vars)) else {
            panic!("expected an error for {path}");
        };
        assert_eq!(
            error,
            format!("cluster: WORKERD_CLUSTER_KEY_FILE={path}: {reason}")
        );
    }
}

#[cfg(unix)]
#[test]
fn cluster_environment_refuses_a_fifo_as_the_key_file_without_blocking() {
    let path =
        std::env::temp_dir().join(format!("workerd-cluster-key-fifo-{}", std::process::id()));
    let c_path = std::ffi::CString::new(path.to_str().unwrap()).unwrap();
    // SAFETY: `c_path` is a valid NUL-terminated path.
    assert_eq!(unsafe { libc::mkfifo(c_path.as_ptr(), 0o600) }, 0);
    let vars = [(CLUSTER_KEY_FILE, path.to_str().unwrap())];
    let result = cluster_config(Some(("", ""))).fill(env(&vars));
    let _ = std::fs::remove_file(&path);
    let Err(error) = result else {
        panic!("expected an error");
    };
    assert!(error.ends_with(": not a regular file"), "{error}");
}

#[test]
fn cluster_environment_reports_an_unreadable_key_file() {
    let vars = [(CLUSTER_KEY_FILE, "/nonexistent/workerd-cluster-key")];
    let Err(error) = cluster_config(Some(("", ""))).fill(env(&vars)) else {
        panic!("expected an error");
    };
    assert!(
        error.starts_with("cluster: WORKERD_CLUSTER_KEY_FILE=/nonexistent/workerd-cluster-key: "),
        "{error}"
    );
}

#[test]
fn cluster_address_joins_a_config_without_cluster_with_defaults() {
    let config = joinable_config().join(Some("10.0.0.4:7000"), None).unwrap();
    let message = read(&config);
    let cluster = message
        .get_root::<config::Reader>()
        .unwrap()
        .get_cluster()
        .unwrap();
    assert_eq!(cluster.get_address().unwrap(), "10.0.0.4:7000");
    assert!(!cluster.has_advertise());
    assert!(!cluster.has_bucket_options());
    assert_eq!(cluster.get_stop_timeout_ms(), 8000);
    assert!(!cluster.get_unsafe_public_address());
    // The key and bucket are left to the environment.
    let vars = [(CLUSTER_KEY, KEY), (CLUSTER_BUCKET, "s3://bucket/prefix")];
    let config = config.fill(env(&vars)).unwrap();
    assert_eq!(
        cluster_of(&config),
        (KEY.to_owned(), "s3://bucket/prefix".to_owned())
    );
}

#[test]
fn cluster_address_leaves_a_config_with_cluster_unchanged() {
    let config = cluster_config(Some((KEY, "s3://bucket/prefix")));
    let before = config.0.clone();
    let config = config
        .join(Some("10.0.0.4:7000"), Some("10.0.0.4:7000"))
        .unwrap();
    assert_eq!(config.0, before);
}

#[test]
fn without_cluster_address_a_config_without_cluster_is_unchanged() {
    let config = cluster_config(None);
    let before = config.0.clone();
    let config = config.join(None, None).unwrap();
    assert_eq!(config.0, before);
    assert!(
        !read(&config)
            .get_root::<config::Reader>()
            .unwrap()
            .has_cluster()
    );
}

#[test]
fn cluster_advertise_without_a_cluster_is_an_error() {
    let Err(error) = cluster_config(None).join(None, Some("10.0.0.4:7000")) else {
        panic!("expected an error");
    };
    assert_eq!(
        error,
        "--cluster-advertise: the config has no cluster. Pass --cluster-addr to join one."
    );
}

#[test]
fn a_config_joins_and_takes_the_environment_in_one_pass() {
    let key = "test-cluster-shared-secret-of-32-bytes";
    let vars = [(CLUSTER_KEY, key), (CLUSTER_BUCKET, "s3://bucket/prefix")];
    let var = env(&vars);
    let flags = ClusterFlags {
        address: Some("10.0.0.4:7000"),
        advertise: None,
        can_join: true,
    };
    let config = joinable_config().with_cluster(&flags, Some(&var)).unwrap();
    assert_eq!(
        cluster_of(&config),
        (key.to_owned(), "s3://bucket/prefix".to_owned())
    );
    // Without the environment, the joined cluster's key is missing.
    let Err(ClusterError::Environment(error)) =
        joinable_config().with_cluster(&flags, Some(&env(&[])))
    else {
        panic!("expected an environment error");
    };
    assert!(error.starts_with("cluster.key is not set."), "{error}");
}

#[test]
fn a_config_without_cluster_storage_cannot_join_a_cluster() {
    let expected = "--cluster-addr: the config has no writable disk service named `do-storage`, \
                    which holds the Durable Objects of the cluster's deployments on each \
                    instance, so it cannot join a cluster. Add one, or serve the wrangler config \
                    itself, which defines it when joined.";
    // None at all, as in `cluster_config`, or one by that name that is not a disk or is read-only.
    let storage = |disk: bool| {
        let mut message = capnp::message::Builder::new_default();
        let mut services = message.init_root::<config::Builder>().init_services(1);
        let mut service = services.reborrow().get(0);
        service.set_name(CLUSTER_STORAGE);
        if disk {
            service.init_disk().set_path("/var/lib/workerd/do");
        } else {
            service.init_external().set_address("10.0.0.5:80");
        }
        Config(words_from_bytes(&capnp::serialize::write_message_to_words(
            &message,
        )))
    };
    for config in [cluster_config(None), storage(false), storage(true)] {
        let Err(error) = config.join(Some("10.0.0.4:7000"), None) else {
            panic!("expected an error");
        };
        assert_eq!(error, expected);
    }
    // A config with a cluster is not checked: it is joined as written.
    let config = cluster_config(Some((KEY, "s3://bucket/prefix")));
    assert!(config.join(Some("10.0.0.4:7000"), None).is_ok());
}

#[test]
fn a_compiled_config_without_cluster_cannot_join_one() {
    let flags = ClusterFlags {
        address: Some("10.0.0.4:7000"),
        advertise: None,
        can_join: false,
    };
    let Err(ClusterError::Usage(error)) = cluster_config(None).with_cluster(&flags, None) else {
        panic!("expected a usage error");
    };
    assert!(
        error.starts_with(
            "--cluster-addr: the config compiled into this executable has no cluster, so it \
             cannot join one."
        ),
        "{error}"
    );
    // One with a cluster keeps it: --cluster-addr overrides its address.
    let config = cluster_config(Some(("k", "s3://b")));
    let before = config.0.clone();
    assert_eq!(config.with_cluster(&flags, None).unwrap().0, before);
}
