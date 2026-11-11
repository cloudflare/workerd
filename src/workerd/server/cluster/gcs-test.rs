// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::collections::BTreeSet;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::Mutex;

use super::*;
use crate::Bucket;
use crate::fake_http::Server;
use crate::fake_http::reply;

const TEST_KEY: &str = include_str!("google_auth-test.pem");

fn env(vars: &[(&str, &str)]) -> impl Fn(&str) -> Option<String> + use<> {
    let vars: Vec<(String, String)> = vars
        .iter()
        .map(|(name, value)| ((*name).to_owned(), (*value).to_owned()))
        .collect();
    move |name| vars.iter().find(|(n, _)| n == name).map(|(_, v)| v.clone())
}

fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}

#[test]
fn urls() {
    let parse = |url| parse_url(url).ok();
    assert_eq!(parse("gs://b/p/q"), Some(("b".into(), "p/q".into())));
    assert_eq!(parse("gs://b"), Some(("b".into(), String::new())));
    assert_eq!(parse("gs:///p"), None);
    assert_eq!(parse("gs://"), None);
    assert_eq!(parse("s3://b/p"), None);
}

#[test]
fn a_gs_bucket_takes_no_options() {
    for option in ["aws_endpoint", "aws_region", "aws_access_key_id"] {
        let options = [(option.to_owned(), "x".to_owned())];
        let Err(Error::Config(message)) = Gcs::with_env("gs://b/p", &options, &env(&[]), None)
        else {
            panic!("{option} was accepted");
        };
        assert!(message.contains(option), "{message}");
    }
}

#[test]
fn where_requests_and_tokens_go() {
    let open = |vars: &[(&str, &str)]| Gcs::with_env("gs://b/p", &[], &env(vars), None).unwrap().0;
    let describe = |gcs: &Gcs| {
        gcs.dialect()
            .credentials
            .as_ref()
            .map(Credentials::describe)
    };
    let metadata = |host: &str| {
        Some(format!(
            "metadata server http://{host}/computeMetadata/v1/instance/service-accounts/default/token"
        ))
    };

    // On Google Cloud: the metadata server. AWS settings play no part.
    let gcs = open(&[("AWS_ENDPOINT_URL", "http://s3"), ("AWS_REGION", "auto")]);
    assert_eq!(gcs.endpoint(), "https://storage.googleapis.com");
    assert_eq!(describe(&gcs), metadata("169.254.169.254"));
    let gcs = open(&[("GCE_METADATA_HOST", "127.0.0.1:8080")]);
    assert_eq!(describe(&gcs), metadata("127.0.0.1:8080"));

    // An emulator takes no token unless a source is named.
    let gcs = open(&[("STORAGE_EMULATOR_HOST", "localhost:9023")]);
    assert_eq!(gcs.endpoint(), "http://localhost:9023");
    assert_eq!(describe(&gcs), None);
    let gcs = open(&[
        ("STORAGE_EMULATOR_HOST", "https://emulator/"),
        ("GCE_METADATA_HOST", "127.0.0.1:8080"),
    ]);
    assert_eq!(gcs.endpoint(), "https://emulator");
    assert_eq!(describe(&gcs), metadata("127.0.0.1:8080"));

    // Empty variables are unset.
    let gcs = open(&[
        ("STORAGE_EMULATOR_HOST", ""),
        ("GOOGLE_APPLICATION_CREDENTIALS", ""),
    ]);
    assert_eq!(gcs.endpoint(), "https://storage.googleapis.com");
    assert_eq!(describe(&gcs), metadata("169.254.169.254"));

    let vars = env(&[("GOOGLE_APPLICATION_CREDENTIALS", "/nonexistent/key.json")]);
    assert!(matches!(
        Gcs::with_env("gs://b/p", &[], &vars, None),
        Err(Error::Config(_))
    ));
}

/// An empty directory of the test's own.
fn scratch(name: &str) -> PathBuf {
    let base = std::env::var("TEST_TMPDIR").map_or_else(|_| std::env::temp_dir(), PathBuf::from);
    let dir = base.join(format!("gcs-test-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

const AUTHORIZED_USER: &str = r#"{"type": "authorized_user", "client_id": "c",
    "client_secret": "s", "refresh_token": "r"}"#;

#[test]
fn gclouds_credentials_file_comes_before_the_metadata_server() {
    let home = scratch("home");
    let config = scratch("config");
    let gcloud = home.join(".config").join("gcloud");
    std::fs::create_dir_all(&gcloud).unwrap();
    let in_home = gcloud.join("application_default_credentials.json");
    let in_config = config.join("application_default_credentials.json");
    let appdata = home.join(".config");
    let (home, config) = (home.to_str().unwrap(), config.to_str().unwrap());
    // `$HOME/.config/gcloud`, and `%APPDATA%\gcloud` on Windows: the same
    // directory.
    let user_vars = [("HOME", home), ("APPDATA", appdata.to_str().unwrap())];
    let with = |more: &[(&'static str, &str)]| -> Vec<(&str, String)> {
        let all = user_vars.iter().chain(more);
        all.map(|(name, value)| (*name, (*value).to_owned()))
            .collect()
    };
    let user = Some("authorized_user credentials".to_owned());

    // No file: the metadata server.
    assert_eq!(describe(&with(&[])).unwrap(), Some(metadata()));
    std::fs::write(&in_home, AUTHORIZED_USER).unwrap();
    assert_eq!(describe(&with(&[])).unwrap(), user);
    // CLOUDSDK_CONFIG moves gcloud's configuration, and with it the file.
    let moved = with(&[("CLOUDSDK_CONFIG", config)]);
    assert_eq!(describe(&moved).unwrap(), Some(metadata()));
    std::fs::write(&in_config, AUTHORIZED_USER).unwrap();
    assert_eq!(describe(&moved).unwrap(), user);
    // GOOGLE_APPLICATION_CREDENTIALS comes first.
    let key = scratch("key").join("key.json");
    let file = format!(
        r#"{{"type": "service_account", "client_email": "sa@p", "private_key": {}}}"#,
        serde_json::to_string(TEST_KEY).unwrap()
    );
    std::fs::write(&key, file).unwrap();
    let named = with(&[("GOOGLE_APPLICATION_CREDENTIALS", key.to_str().unwrap())]);
    assert_eq!(
        describe(&named).unwrap(),
        Some("service account sa@p".into())
    );
    // An emulator does not read it.
    let emulator = with(&[("STORAGE_EMULATOR_HOST", "localhost:9023")]);
    assert_eq!(describe(&emulator).unwrap(), None);

    // A file of a type that is not supported is an error, not a reason to
    // try the metadata server.
    std::fs::write(&in_home, r#"{"type": "external_account"}"#).unwrap();
    let Err(Error::Config(message)) = describe(&with(&[])) else {
        panic!("an external_account file was accepted");
    };
    assert_eq!(
        message,
        format!(
            "{}: credentials of type \"external_account\" are not supported: use a service \
             account key, authorized_user or impersonated_service_account credentials, or the \
             metadata server",
            in_home.display()
        )
    );
    // So is one that is there but cannot be read.
    std::fs::remove_file(&in_home).unwrap();
    std::fs::create_dir(&in_home).unwrap();
    assert!(matches!(describe(&with(&[])), Err(Error::Config(_))));
}

/// The source a gs:// bucket's tokens come from, under the environment
/// `vars`.
fn describe(vars: &[(&str, String)]) -> Result<Option<String>> {
    let vars: Vec<(&str, &str)> = vars.iter().map(|(n, v)| (*n, v.as_str())).collect();
    let gcs = Gcs::with_env("gs://b/p", &[], &env(&vars), None).map(|(gcs, _)| gcs);
    gcs.map(|gcs| {
        gcs.dialect()
            .credentials
            .as_ref()
            .map(Credentials::describe)
    })
}

fn metadata() -> String {
    let url = "http://169.254.169.254/computeMetadata/v1/instance/service-accounts/default/token";
    format!("metadata server {url}")
}

#[test]
fn a_gcloud_file_out_of_reach_is_not_there() {
    let vars = |home: &str| vec![("HOME", home.to_owned()), ("APPDATA", home.to_owned())];
    // A home that is a file.
    let file = scratch("home-file").join("file");
    std::fs::write(&file, "").unwrap();
    assert_eq!(
        describe(&vars(file.to_str().unwrap())).unwrap(),
        Some(metadata())
    );
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(describe(&vars("/dev/null")).unwrap(), Some(metadata()));
        // A home that this user may not enter, as /root with mode 0700 is to
        // a container's other users.
        let home = scratch("home-closed");
        let gcloud = home.join(".config").join("gcloud");
        std::fs::create_dir_all(&gcloud).unwrap();
        let path = gcloud.join("application_default_credentials.json");
        std::fs::write(&path, AUTHORIZED_USER).unwrap();
        std::fs::set_permissions(&home, std::fs::Permissions::from_mode(0o000)).unwrap();
        let reachable = std::fs::metadata(&path).is_ok();
        let found = describe(&vars(home.to_str().unwrap()));
        std::fs::set_permissions(&home, std::fs::Permissions::from_mode(0o700)).unwrap();
        // Root enters it anyway.
        if !reachable {
            assert_eq!(found.unwrap(), Some(metadata()));
        }
    }
}

/// An `impersonated_service_account` file for `target`, whose source is
/// `source` and whose tokens come from `generateAccessToken` at `base`.
fn impersonated(target: &str, source: &serde_json::Value, base: &str) -> serde_json::Value {
    let path = format!("/v1/projects/-/serviceAccounts/{target}:generateAccessToken");
    serde_json::json!({
        "type": "impersonated_service_account",
        "service_account_impersonation_url": format!("{base}{path}"),
        "source_credentials": source,
    })
}

#[test]
fn a_service_account_to_impersonate_from_the_environment() {
    let target = "t@p.iam.gserviceaccount.com";
    let named = [
        ("GCE_METADATA_HOST", "127.0.0.1:8080".to_owned()),
        ("GOOGLE_IMPERSONATE_SERVICE_ACCOUNT", target.to_owned()),
    ];
    assert_eq!(
        describe(&named).unwrap().unwrap(),
        format!(
            "service account {target}, impersonated with metadata server \
             http://127.0.0.1:8080/computeMetadata/v1/instance/service-accounts/default/token"
        )
    );
    let bad = [("GOOGLE_IMPERSONATE_SERVICE_ACCOUNT", "t@p/x".to_owned())];
    let Err(Error::Config(message)) = describe(&bad) else {
        panic!("a bad address was accepted");
    };
    assert_eq!(
        message,
        "GOOGLE_IMPERSONATE_SERVICE_ACCOUNT: \"t@p/x\" is not a service account's email address \
         or unique ID"
    );
    // An emulator ignores it unless a source is named for the emulator.
    let emulator = ("STORAGE_EMULATOR_HOST", "localhost:9023".to_owned());
    let unnamed = [emulator.clone(), named[1].clone()];
    assert_eq!(describe(&unnamed).unwrap(), None);
    let with_source = [emulator, named[0].clone(), named[1].clone()];
    assert_eq!(describe(&with_source).unwrap(), describe(&named).unwrap());
    // Nor does a file that impersonates already take another.
    let path = scratch("impersonated").join("credentials.json");
    let user: serde_json::Value = serde_json::from_str(AUTHORIZED_USER).unwrap();
    let file = impersonated(target, &user, "https://iamcredentials.googleapis.com");
    std::fs::write(&path, file.to_string()).unwrap();
    let file = (
        "GOOGLE_APPLICATION_CREDENTIALS",
        path.to_str().unwrap().to_owned(),
    );
    assert_eq!(
        describe(std::slice::from_ref(&file)).unwrap().unwrap(),
        format!("service account {target}, impersonated with authorized_user credentials")
    );
    let other = "u@p.iam.gserviceaccount.com".to_owned();
    let twice = [file, ("GOOGLE_IMPERSONATE_SERVICE_ACCOUNT", other)];
    let Err(Error::Config(message)) = describe(&twice) else {
        panic!("impersonating twice was accepted");
    };
    assert_eq!(
        message,
        format!("GOOGLE_IMPERSONATE_SERVICE_ACCOUNT: the credentials impersonate {target} already")
    );
}

#[test]
fn an_impersonated_token_reaches_the_bucket() {
    runtime().block_on(async {
        let generate = "/v1/projects/-/serviceAccounts/t@p:generateAccessToken";
        let server = Server::serving(move |request, _| {
            let body = match request.path.as_str() {
                "/token" => serde_json::json!({ "access_token": "source", "expires_in": 3600 }),
                path if path == generate => serde_json::json!({
                    "accessToken": "impersonated",
                    "expireTime": "2999-01-01T00:00:00Z",
                }),
                _ => return reply(404, ""),
            };
            reply(200, &body.to_string())
        })
        .await;
        let base = format!("http://{}", server.host);
        let source = serde_json::json!({
            "type": "authorized_user",
            "client_id": "c",
            "client_secret": "s",
            "refresh_token": "r",
            "token_uri": format!("{base}/token"),
            "quota_project_id": "qp-source",
        });
        let mut file = impersonated("t@p", &source, &base);
        file["quota_project_id"] = "qp-bucket".into();
        let path = scratch("impersonated-bucket").join("credentials.json");
        std::fs::write(&path, file.to_string()).unwrap();
        let vars = env(&[
            ("STORAGE_EMULATOR_HOST", &server.host),
            ("GOOGLE_APPLICATION_CREDENTIALS", path.to_str().unwrap()),
        ]);
        let (gcs, prefix) = Gcs::with_env("gs://bkt/pre", &[], &vars, None).unwrap();
        let bucket = Bucket::new(gcs, &prefix);
        assert!(bucket.get("k").await.unwrap().is_none());
        // The source's quota project is billed for the IAM call, the file's
        // for the bucket's.
        let requests: Vec<_> = server
            .take()
            .into_iter()
            .map(|r| {
                let header = |name| r.header(name).unwrap_or("").to_owned();
                let headers = (header("authorization"), header("x-goog-user-project"));
                (r.method.clone(), r.path.clone(), headers)
            })
            .collect();
        let sent = |method: &str, path: &str, token: &str, project: &str| {
            (
                method.to_owned(),
                path.to_owned(),
                (token.to_owned(), project.to_owned()),
            )
        };
        assert_eq!(
            requests,
            [
                sent("POST", "/token", "", ""),
                sent("POST", generate, "Bearer source", "qp-source"),
                sent("GET", "/bkt/pre/k", "Bearer impersonated", "qp-bucket"),
            ]
        );
    });
}

#[test]
fn preconditions_name_generations() {
    let header = |guard| Google::precondition(&guard).ok().map(|(_, value)| value);
    assert_eq!(
        Google::precondition(&Guard::Absent).unwrap(),
        ("x-goog-if-generation-match".to_owned(), "0".to_owned())
    );
    let generation = "1700000000000001";
    assert_eq!(
        header(Guard::Match(Version(generation.into()))).as_deref(),
        Some(generation)
    );
    // Not a version this store answers: an S3 ETag, nothing, and 0, which
    // would mean that the object must not exist.
    for version in ["\"0\"", "", "0", "00", "12a"] {
        assert_eq!(header(Guard::Match(Version(version.into()))), None);
    }
}

#[test]
fn versions_are_generations() {
    let response = |generation: &str| HttpResponse {
        status: 200,
        etag: "\"e\"".into(),
        generation: generation.into(),
        date: String::new(),
        metadata_flavor: String::new(),
        body: Vec::new(),
    };
    assert_eq!(Google::version(&response("17")), "17");
    // None, which the client refuses.
    assert_eq!(Google::version(&response("")), "");
    assert_eq!(Google::version(&response("x")), "");
}

/// A bucket `bkt` behind tokens from the metadata server: each request for
/// one answers `t<n>`, and the bucket takes the tokens in `accepted`, unless
/// it `refuse`s all.
#[derive(Default)]
struct Fake {
    issued: usize,
    accepted: BTreeSet<String>,
    refuse: bool,
}

const METADATA: &str = "/computeMetadata/v1/instance/service-accounts/default/token";

/// A client of the bucket at `fake`, with tokens from its metadata server.
async fn client(fake: &Arc<Mutex<Fake>>) -> (Bucket, Server) {
    let state = fake.clone();
    let server = Server::serving(move |request, _| {
        let mut fake = state.lock().unwrap();
        if request.path == METADATA {
            fake.issued += 1;
            let token = format!("t{}", fake.issued);
            fake.accepted.insert(token.clone());
            let body = serde_json::json!({ "access_token": token, "expires_in": 3600 });
            return reply(200, &body.to_string());
        }
        let token = request
            .header("authorization")
            .and_then(|v| v.strip_prefix("Bearer "));
        if !fake.refuse && token.is_some_and(|token| fake.accepted.contains(token)) {
            reply(404, "")
        } else {
            reply(401, "")
        }
    })
    .await;
    let vars = env(&[
        ("STORAGE_EMULATOR_HOST", &server.host),
        ("GCE_METADATA_HOST", &server.host),
    ]);
    let (gcs, prefix) = Gcs::with_env("gs://bkt/pre", &[], &vars, None).unwrap();
    (Bucket::new(gcs, &prefix), server)
}

/// The path and the `authorization` header of each request.
fn sent(server: &Server) -> Vec<(String, String)> {
    let header = |r: &crate::fake_http::Request| r.header("authorization").unwrap_or("").to_owned();
    server
        .take()
        .iter()
        .map(|r| (r.path.clone(), header(r)))
        .collect()
}

fn bearer(token: &str) -> String {
    format!("Bearer {token}")
}

#[test]
fn a_refused_token_is_replaced_once() {
    runtime().block_on(async {
        let fake = Arc::new(Mutex::new(Fake::default()));
        let (bucket, server) = client(&fake).await;
        assert!(bucket.get("k").await.unwrap().is_none());
        // The bucket stops taking the token: the call fetches another.
        fake.lock().unwrap().accepted.clear();
        assert!(bucket.get("k").await.unwrap().is_none());
        let object = "/bkt/pre/k".to_owned();
        assert_eq!(
            sent(&server),
            [
                (METADATA.into(), String::new()),
                (object.clone(), bearer("t1")),
                (object.clone(), bearer("t1")),
                (METADATA.into(), String::new()),
                (object.clone(), bearer("t2")),
            ]
        );
        // A second token refused soon after fails the call, without a third.
        fake.lock().unwrap().accepted.clear();
        let Err(Error::Http(message)) = bucket.get("k").await else {
            panic!("expected an error");
        };
        assert_eq!(message, "GET pre/k: 401 ");
        assert_eq!(sent(&server), [(object, bearer("t2"))]);
    });
}

#[test]
fn a_replacement_refused_in_the_same_call_is_not_sent_again() {
    runtime().block_on(async {
        let fake = Arc::new(Mutex::new(Fake::default()));
        let (bucket, server) = client(&fake).await;
        assert!(bucket.get("k").await.unwrap().is_none());
        server.take();
        // Both the token and its replacement are refused within one call.
        fake.lock().unwrap().refuse = true;
        let Err(Error::Http(message)) = bucket.get("k").await else {
            panic!("expected an error");
        };
        assert_eq!(message, "GET pre/k: 401 ");
        let object = "/bkt/pre/k".to_owned();
        assert_eq!(
            sent(&server),
            [
                (object.clone(), bearer("t1")),
                (METADATA.into(), String::new()),
                (object, bearer("t2")),
            ]
        );
        // The next call does not send the refused replacement: it waits out
        // the pause.
        let Err(Error::Http(message)) = bucket.get("k").await else {
            panic!("expected an error");
        };
        assert!(
            message.contains("the bucket refused two tokens"),
            "{message}"
        );
        assert!(sent(&server).is_empty());
    });
}
