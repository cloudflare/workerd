// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::collections::BTreeSet;
use std::sync::Arc;
use std::sync::Mutex;

use super::*;
use crate::Bucket;
use crate::fake_http::Server;
use crate::fake_http::reply;

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
