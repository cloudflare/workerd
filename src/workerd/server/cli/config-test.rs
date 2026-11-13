use super::*;

fn read(config: &Config) -> capnp::message::Reader<capnp::serialize::OwnedSegments> {
    capnp::serialize::read_message(bytes_from_words(&config.0).as_slice(), reader_options())
        .unwrap()
}

#[test]
fn python_baseline_config_has_one_python_worker() {
    use workerd_capnp::service;
    use workerd_capnp::worker;

    let message = read(&Config::python_baseline("python_workers_20250116"));
    let config = message.get_root::<config::Reader>().unwrap();
    let services = config.get_services().unwrap();
    assert_eq!(services.len(), 1);
    let service = services.get(0);
    assert_eq!(service.get_name().unwrap(), "main");
    let service::Which::Worker(worker) = service.which().unwrap() else {
        panic!("expected a worker service")
    };
    let worker = worker.unwrap();
    let flags = worker.get_compatibility_flags().unwrap();
    assert_eq!(flags.get(0).unwrap(), "python_workers_20250116");
    assert_eq!(flags.get(1).unwrap(), "python_workers");
    let worker::Which::Modules(modules) = worker.which().unwrap() else {
        panic!("expected a modules worker")
    };
    assert_eq!(modules.unwrap().get(0).get_name().unwrap(), "main.py");
}

#[test]
fn config_bytes_are_checked() {
    let good = Config::python_baseline("python_workers");
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
    let config = cluster_config(Some(("", "")))
        .with_cluster_environment(env(&vars))
        .unwrap();
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
    let config = config.with_cluster_environment(env(&vars)).unwrap();
    assert_eq!(config.0, before);
}

#[test]
fn cluster_environment_leaves_a_config_without_cluster_unchanged() {
    let vars = [(CLUSTER_KEY, KEY), (CLUSTER_BUCKET, "s3://bucket/prefix")];
    let config = cluster_config(None);
    let before = config.0.clone();
    let config = config.with_cluster_environment(env(&vars)).unwrap();
    assert_eq!(config.0, before);
}

#[test]
fn cluster_environment_reads_the_key_file_without_trailing_whitespace() {
    let file = TempFile::new("workerd-cluster-key-test", format!("{KEY}\n"));
    let vars = [(CLUSTER_KEY_FILE, file.path())];
    let config = cluster_config(Some(("", "s3://bucket/prefix")))
        .with_cluster_environment(env(&vars))
        .unwrap();
    assert_eq!(cluster_of(&config).0, KEY);
}

#[test]
fn cluster_environment_trims_the_key_variable() {
    let key = format!("{KEY} \n");
    let vars = [(CLUSTER_KEY, key.as_str())];
    let config = cluster_config(Some(("", "s3://bucket/prefix")))
        .with_cluster_environment(env(&vars))
        .unwrap();
    assert_eq!(cluster_of(&config).0, KEY);
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
    ] {
        let Err(error) = cluster_config(Some(cluster)).with_cluster_environment(env(&vars)) else {
            panic!("expected an error for {cluster:?} and {vars:?}");
        };
        assert_eq!(error, expected);
    }
}

#[test]
fn cluster_environment_refuses_a_key_file_that_is_not_text() {
    let file = TempFile::new("workerd-cluster-key-binary-test", [0xff, 0xfe, 0x00, 0x80]);
    let vars = [(CLUSTER_KEY_FILE, file.path())];
    let Err(error) = cluster_config(Some(("", ""))).with_cluster_environment(env(&vars)) else {
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
    let Err(error) = cluster_config(Some(("", ""))).with_cluster_environment(env(&vars)) else {
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
        let Err(error) = cluster_config(Some(("", ""))).with_cluster_environment(env(&vars)) else {
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
    let result = cluster_config(Some(("", ""))).with_cluster_environment(env(&vars));
    let _ = std::fs::remove_file(&path);
    let Err(error) = result else {
        panic!("expected an error");
    };
    assert!(error.ends_with(": not a regular file"), "{error}");
}

#[test]
fn cluster_environment_reports_an_unreadable_key_file() {
    let vars = [(CLUSTER_KEY_FILE, "/nonexistent/workerd-cluster-key")];
    let Err(error) = cluster_config(Some(("", ""))).with_cluster_environment(env(&vars)) else {
        panic!("expected an error");
    };
    assert!(
        error.starts_with("cluster: WORKERD_CLUSTER_KEY_FILE=/nonexistent/workerd-cluster-key: "),
        "{error}"
    );
}

#[test]
fn cluster_address_joins_a_config_without_cluster_with_defaults() {
    let config = cluster_config(None)
        .joining_cluster(Some("10.0.0.4:7000"), None)
        .unwrap();
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
    let config = config.with_cluster_environment(env(&vars)).unwrap();
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
        .joining_cluster(Some("10.0.0.4:7000"), Some("10.0.0.4:7000"))
        .unwrap();
    assert_eq!(config.0, before);
}

#[test]
fn without_cluster_address_a_config_without_cluster_is_unchanged() {
    let config = cluster_config(None);
    let before = config.0.clone();
    let config = config.joining_cluster(None, None).unwrap();
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
    let Err(error) = cluster_config(None).joining_cluster(None, Some("10.0.0.4:7000")) else {
        panic!("expected an error");
    };
    assert_eq!(
        error,
        "--cluster-advertise: the config has no cluster. Pass --cluster-addr to join one."
    );
}
