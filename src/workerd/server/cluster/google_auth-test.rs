// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::collections::VecDeque;

use ring::signature::RSA_PKCS1_2048_8192_SHA256;
use ring::signature::UnparsedPublicKey;
use tokio::task::JoinSet;

use super::*;
use crate::fake_http::Answer;
use crate::fake_http::Server;

const TEST_KEY: &str = include_str!("google_auth-test.pem");

fn service_account(fields: &str) -> String {
    let key = serde_json::to_string(TEST_KEY).unwrap();
    format!(
        r#"{{"type": "service_account", "client_email": "sa@p.iam.gserviceaccount.com",
             "private_key": {key} {fields}}}"#
    )
}

fn test_key() -> RsaKeyPair {
    let der = PrivatePkcs8KeyDer::from_pem_slice(TEST_KEY.as_bytes()).unwrap();
    RsaKeyPair::from_pkcs8(der.secret_pkcs8_der()).unwrap()
}

fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}

fn parse(text: &str) -> std::result::Result<Source, String> {
    parse_file(text)
}

#[test]
fn a_token_is_replaced_before_it_expires() {
    let now = Instant::now();
    let token = Token::new("t".into(), now, Duration::from_hours(1));
    assert_eq!(token.refresh_at - now, Duration::from_mins(55));
    assert_eq!(token.expires_at - now, Duration::from_hours(1));
    // A short-lived token, halfway through its lifetime.
    let token = Token::new("t".into(), now, Duration::from_secs(400));
    assert_eq!(token.refresh_at - now, Duration::from_secs(200));
    let token = Token::new("t".into(), now, Duration::ZERO);
    assert_eq!(token.refresh_at, now);
}

#[test]
fn token_answers() {
    let now = Instant::now();
    let body = br#"{"access_token": "ya29.x", "expires_in": 3599, "token_type": "Bearer"}"#;
    let token = parse_token(body, now).unwrap();
    assert_eq!(token.value, "ya29.x");
    assert_eq!(token.expires_at - now, Duration::from_secs(3599));
    assert!(parse_token(br#"{"access_token": "", "expires_in": 1}"#, now).is_err());
    assert!(parse_token(br#"{"expires_in": 1}"#, now).is_err());
    assert!(parse_token(b"<html>", now).is_err());
    assert!(parse_token(br#"{"access_token": "t", "expires_in": -1}"#, now).is_err());
    assert!(parse_token(br#"{"access_token": "t", "expires_in": 1e30}"#, now).is_err());
    let Err(message) = parse_token(br#"{"access_token": "t", "expires_in": 0}"#, now) else {
        panic!("an expired token was accepted");
    };
    assert_eq!(message, "the answer's token has expired");
    // However long a token claims to last, it is replaced within a day.
    let body = format!(r#"{{"access_token": "t", "expires_in": {}}}"#, u64::MAX);
    let token = parse_token(body.as_bytes(), now).unwrap();
    assert_eq!(token.expires_at - now, Duration::from_hours(24));
    assert_eq!(token.refresh_at - now, Duration::from_mins(24 * 60 - 5));
}

#[test]
fn a_token_does_not_show_in_debug_output() {
    let token = Token::new(
        "ya29.secret".into(),
        Instant::now(),
        Duration::from_hours(1),
    );
    let shown = format!("{token:?}");
    assert!(!shown.contains("ya29"), "{shown}");
    assert!(shown.contains("<redacted>"), "{shown}");
}

#[test]
fn error_answers() {
    let oauth = br#"{"error": "invalid_grant", "error_description": "Invalid JWT"}"#;
    assert_eq!(error_text(oauth), "invalid_grant: Invalid JWT");
    assert_eq!(
        error_text(br#"{"error": "invalid_grant"}"#),
        "invalid_grant"
    );
    let api = br#"{"error": {"code": 403, "status": "PERMISSION_DENIED", "message": "denied"}}"#;
    assert_eq!(error_text(api), "PERMISSION_DENIED: denied");
    assert_eq!(
        error_text(br#"{"error": {"code": 404, "message": "gone"}}"#),
        "gone"
    );
    // Anything else of a body stays out of messages: it may echo the
    // request's credentials.
    for body in [
        &b"Not Found"[..],
        b"Bearer ya29.secret",
        br#"{"authorization": "Bearer ya29.secret"}"#,
        br#"{"error": 401, "echo": "Bearer ya29.secret"}"#,
        b"",
    ] {
        assert_eq!(error_text(body), "", "{}", String::from_utf8_lossy(body));
    }
    let long = "x".repeat(10_000);
    let oauth = format!(r#"{{"error": "e", "error_description": "{long}"}}"#);
    assert_eq!(error_text(oauth.as_bytes()).chars().count(), 200);
}

#[test]
fn token_endpoints_are_https_unless_on_loopback() {
    for uri in [
        "https://oauth2.googleapis.com/token",
        "https://example.com:8443/t",
        "HTTPS://Example.COM/token",
        "http://127.0.0.1:1/token",
        "http://127.8.0.1/token",
        "http://localhost:8080/token",
        "http://localhost",
        "HTTP://LOCALHOST/token",
        "http://[::1]:9/token",
        "http://[::1]/token",
    ] {
        assert_eq!(check_token_uri(uri), Ok(()), "{uri}");
    }
    let cleartext = "must be https, or http on a loopback address";
    let userinfo = "must not hold a user name or password";
    let relative = "is not an absolute URL";
    for (uri, why) in [
        ("http://oauth2.googleapis.com/token", cleartext),
        ("http://10.0.0.1/token", cleartext),
        ("http://localhost.evil.example/token", cleartext),
        ("http://127.0.0.1.evil.example/token", cleartext),
        ("http://localhost./token", cleartext),
        ("http://127.0.0.1./token", cleartext),
        ("http://[::2]/token", cleartext),
        ("http://[::ffff:127.0.0.1]/token", cleartext),
        ("ftp://localhost/token", cleartext),
        // The host is attacker.example: what precedes `@` is userinfo.
        ("http://localhost:80@attacker.example/token", userinfo),
        ("http://localhost@attacker.example/token", userinfo),
        ("http://user@localhost/token", userinfo),
        (
            "https://user:password@oauth2.googleapis.com/token",
            userinfo,
        ),
        ("oauth2.googleapis.com/token", relative),
        ("/token", relative),
        ("", relative),
    ] {
        assert_eq!(
            check_token_uri(uri),
            Err(format!("token_uri {uri:?} {why}")),
            "{uri}"
        );
    }
    let file = service_account(r#", "token_uri": "http://oauth2.googleapis.com/token""#);
    let Err(message) = parse(&file) else {
        panic!("a cleartext token_uri was accepted");
    };
    assert_eq!(
        message,
        "token_uri \"http://oauth2.googleapis.com/token\" must be https, or http on a loopback \
         address"
    );
}

#[test]
fn credentials_files() {
    let file = service_account(r#", "private_key_id": "k1""#);
    let credentials = Credentials::from_json(&file, Http::new().unwrap()).unwrap();
    assert_eq!(
        credentials.describe(),
        "service account sa@p.iam.gserviceaccount.com"
    );
    let Source::ServiceAccount {
        key_id, token_uri, ..
    } = parse(&file).unwrap()
    else {
        panic!("expected a service account");
    };
    assert_eq!(key_id.as_deref(), Some("k1"));
    assert_eq!(token_uri, TOKEN_URI);
    let file = service_account(r#", "token_uri": "http://127.0.0.1:1/token""#);
    let Source::ServiceAccount { token_uri, .. } = parse(&file).unwrap() else {
        panic!("expected a service account");
    };
    assert_eq!(token_uri, "http://127.0.0.1:1/token");

    for (file, error) in [
        (
            r#"{"type": "service_account", "client_email": "sa@p"}"#.to_owned(),
            "service_account credentials have no private_key",
        ),
        (
            r#"{"type": "service_account", "private_key": "x", "client_email": "sa@p"}"#.to_owned(),
            "private_key: ",
        ),
        (
            r#"{"type": "authorized_user", "client_id": "c", "client_secret": "s",
                "refresh_token": "r"}"#
                .to_owned(),
            "credentials of type \"authorized_user\" are not supported",
        ),
        (
            r#"{"type": "external_account"}"#.to_owned(),
            "credentials of type \"external_account\" are not supported",
        ),
        ("{".to_owned(), "EOF"),
    ] {
        let Err(message) = parse(&file) else {
            panic!("{file} was accepted");
        };
        assert!(message.contains(error), "{message}");
    }
}

#[test]
fn a_service_account_signs_its_assertion() {
    let key = test_key();
    let audience = "https://oauth2.googleapis.com/token";
    let jwt = assertion(&key, "sa@p", Some("k1"), audience, 1_700_000_000).unwrap();
    let parts: Vec<_> = jwt.split('.').collect();
    assert_eq!(parts.len(), 3);
    let decode = |part: &str| -> serde_json::Value {
        serde_json::from_slice(&URL_SAFE_NO_PAD.decode(part).unwrap()).unwrap()
    };
    assert_eq!(
        decode(parts[0]),
        serde_json::json!({ "alg": "RS256", "typ": "JWT", "kid": "k1" })
    );
    assert_eq!(
        decode(parts[1]),
        serde_json::json!({
            "iss": "sa@p",
            "scope": "https://www.googleapis.com/auth/devstorage.read_write",
            "aud": audience,
            "iat": 1_700_000_000,
            "exp": 1_700_003_600,
        })
    );
    let signature = URL_SAFE_NO_PAD.decode(parts[2]).unwrap();
    let public = UnparsedPublicKey::new(&RSA_PKCS1_2048_8192_SHA256, key.public().as_ref());
    let signed = format!("{}.{}", parts[0], parts[1]);
    public.verify(signed.as_bytes(), &signature).unwrap();

    let jwt = assertion(&key, "sa@p", None, audience, 1_700_000_000).unwrap();
    let header = jwt.split('.').next().unwrap();
    assert_eq!(
        decode(header),
        serde_json::json!({ "alg": "RS256", "typ": "JWT" })
    );
}

/// What a scripted token endpoint does with a request.
#[derive(Clone, Copy, Debug)]
enum Step {
    /// Never answers.
    Stall,
    /// Answers after `Script::open`.
    Gated,
    /// An error.
    Fail,
    /// A token, `t<n>` for the endpoint's `n`th request.
    Token,
    Panic,
}

/// A token endpoint that takes `steps` in turn, then `rest` for every
/// request after; it counts the requests.
struct Script {
    steps: Mutex<VecDeque<Step>>,
    rest: Step,
    requests: Mutex<usize>,
    gate: tokio::sync::Semaphore,
}

impl Script {
    fn requests(&self) -> usize {
        *self.requests.lock().unwrap()
    }

    /// Lets one request at `Step::Gated` answer.
    fn open(&self) {
        self.gate.add_permits(1);
    }
}

fn scripted(steps: &[Step], rest: Step, timing: Timing) -> (Credentials, Arc<Script>) {
    let script = Arc::new(Script {
        steps: Mutex::new(steps.iter().copied().collect()),
        rest,
        requests: Mutex::new(0),
        gate: tokio::sync::Semaphore::new(0),
    });
    let endpoint = script.clone();
    let answer: TestEndpoint = Arc::new(move || {
        let script = endpoint.clone();
        let n = {
            let mut requests = script.requests.lock().unwrap();
            *requests += 1;
            *requests
        };
        let step = script
            .steps
            .lock()
            .unwrap()
            .pop_front()
            .unwrap_or(script.rest);
        Box::pin(async move {
            match step {
                Step::Stall => std::future::pending().await,
                Step::Gated => script.gate.acquire().await.unwrap().forget(),
                Step::Fail => return Err(format!("refused request {n}")),
                Step::Token => {}
                Step::Panic => panic!("request {n} panics"),
            }
            Ok(Token::new(
                format!("t{n}"),
                Instant::now(),
                Duration::from_hours(1),
            ))
        })
    });
    let credentials = Credentials::over(Source::Test(answer), Http::new().unwrap(), timing);
    (credentials, script)
}

/// Pauses no test waits through: what a test checks happens before them,
/// or is made to by setting `State::retry_at`.
fn patient() -> Timing {
    Timing {
        attempt: Duration::from_hours(1),
        budget: Duration::from_hours(1),
        pause: Duration::from_millis(1),
        max_pause: Duration::from_millis(1),
        backoff: Duration::from_hours(1),
        max_backoff: Duration::from_hours(3),
    }
}

/// Caches `token`, as a request that answered it would.
fn seed(credentials: &Credentials, token: Token) {
    let mut state = credentials.shared.lock();
    state.token = Some(token);
    state.had_token = true;
}

/// Caches `value`, valid for an hour and due for replacement.
fn cache_due(credentials: &Credentials, value: &str) {
    let now = Instant::now();
    seed(
        credentials,
        Token {
            value: value.into(),
            refresh_at: now,
            expires_at: now + Duration::from_hours(1),
        },
    );
}

/// Ends the pause after the last failed request.
fn end_pause(credentials: &Credentials) {
    credentials.shared.lock().retry_at = Some(Instant::now());
}

/// A real-time limit for what a test waits for, generous enough for a
/// loaded machine.
const PATIENCE: Duration = Duration::from_secs(20);

/// `count` concurrent calls for a token, each of which must answer within
/// `deadline`.
async fn tokens(
    credentials: &Credentials,
    count: usize,
    deadline: Duration,
) -> Vec<Result<String>> {
    let mut calls = JoinSet::new();
    for _ in 0..count {
        let credentials = credentials.clone();
        calls.spawn(async move { tokio::time::timeout(deadline, credentials.token()).await });
    }
    let mut answers = Vec::new();
    while let Some(answer) = calls.join_next().await {
        answers.push(answer.unwrap().expect("a call outlasted its deadline"));
    }
    answers
}

fn values(answers: Vec<Result<String>>) -> Vec<String> {
    answers.into_iter().map(Result::unwrap).collect()
}

/// Waits for `done`, for at most `PATIENCE`.
async fn until(what: &str, done: impl Fn() -> bool) {
    let start = Instant::now();
    while !done() {
        assert!(start.elapsed() < PATIENCE, "waited too long for {what}");
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
}

/// Waits until `count` calls wait for the request under way.
async fn until_waiting(credentials: &Credentials, count: usize) {
    let ended = &credentials.shared.ended;
    until("the calls to wait", || ended.receiver_count() == count).await;
}

fn requesting(credentials: &Credentials) -> bool {
    credentials.shared.lock().requesting
}

#[test]
fn a_valid_token_is_answered_while_its_replacement_stalls() {
    runtime().block_on(async {
        let (credentials, script) = scripted(&[], Step::Stall, patient());
        cache_due(&credentials, "old");
        // Within the 4 s a lease renewal has, while the request stalls.
        let deadline = Duration::from_secs(2);
        let answers = values(tokens(&credentials, 20, deadline).await);
        assert!(answers.iter().all(|t| t == "old"), "{answers:?}");
        assert!(requesting(&credentials));
        assert_eq!(script.requests(), 1);
    });
}

#[test]
fn a_failed_replacement_pauses_before_the_next() {
    runtime().block_on(async {
        let (credentials, script) = scripted(&[], Step::Fail, patient());
        cache_due(&credentials, "old");
        assert_eq!(credentials.token().await.unwrap(), "old");
        until("the request to fail", || !requesting(&credentials)).await;
        // Calls go on with the valid token and start no request until the
        // pause after the failure is over.
        let answers = values(tokens(&credentials, 20, PATIENCE).await);
        assert!(answers.iter().all(|t| t == "old"), "{answers:?}");
        assert!(!requesting(&credentials));
        assert_eq!(script.requests(), 1);
        assert_eq!(credentials.shared.lock().backoff, Duration::from_hours(1));
        assert_eq!(
            credentials.shared.lock().error.as_deref(),
            Some("refused request 1")
        );
        // After it, one call starts the next request; the pause doubles.
        end_pause(&credentials);
        assert_eq!(credentials.token().await.unwrap(), "old");
        until("the request to fail", || !requesting(&credentials)).await;
        assert_eq!(script.requests(), 2);
        assert_eq!(credentials.shared.lock().backoff, Duration::from_hours(2));
        end_pause(&credentials);
        credentials.token().await.unwrap();
        until("the request to fail", || !requesting(&credentials)).await;
        assert_eq!(credentials.shared.lock().backoff, Duration::from_hours(3));
    });
}

#[test]
fn a_request_outlives_the_call_that_started_it() {
    runtime().block_on(async {
        // A replacement, started by a call that is answered at once.
        let (refreshed, first) = scripted(&[], Step::Gated, patient());
        cache_due(&refreshed, "old");
        assert_eq!(refreshed.token().await.unwrap(), "old");
        // A first token, for a call that gives up waiting for it.
        let (abandoned, second) = scripted(&[], Step::Gated, patient());
        let call = tokio::time::timeout(Duration::from_millis(10), abandoned.token());
        assert!(call.await.is_err());
        first.open();
        second.open();
        for credentials in [&refreshed, &abandoned] {
            until("the token", || !requesting(credentials)).await;
            assert_eq!(credentials.token().await.unwrap(), "t1");
        }
        assert_eq!((first.requests(), second.requests()), (1, 1));
    });
}

#[test]
fn calls_without_a_token_share_one_request() {
    runtime().block_on(async {
        let (credentials, script) = scripted(&[], Step::Gated, patient());
        let calls = tokio::spawn({
            let credentials = credentials.clone();
            async move { tokens(&credentials, 20, PATIENCE).await }
        });
        until_waiting(&credentials, 20).await;
        script.open();
        let answers = values(calls.await.unwrap());
        assert!(answers.iter().all(|t| t == "t1"), "{answers:?}");
        assert_eq!(script.requests(), 1);
        // An expired token is waited for as well.
        let now = Instant::now();
        seed(
            &credentials,
            Token {
                value: "old".into(),
                refresh_at: now,
                expires_at: now,
            },
        );
        let calls = tokio::spawn({
            let credentials = credentials.clone();
            async move { tokens(&credentials, 20, PATIENCE).await }
        });
        until_waiting(&credentials, 20).await;
        script.open();
        let answers = values(calls.await.unwrap());
        assert!(answers.iter().all(|t| t == "t2"), "{answers:?}");
        assert_eq!(script.requests(), 2);
    });
}

#[test]
fn a_first_token_is_retried_until_one_is_had() {
    runtime().block_on(async {
        let timing = Timing {
            attempt: Duration::from_millis(50),
            ..patient()
        };
        let script = [Step::Fail, Step::Stall, Step::Fail];
        let (credentials, script) = scripted(&script, Step::Token, timing);
        let answers = values(tokens(&credentials, 10, PATIENCE).await);
        assert!(answers.iter().all(|t| t == "t4"), "{answers:?}");
        assert_eq!(script.requests(), 4);
    });
}

#[test]
fn a_first_token_is_given_up_on() {
    runtime().block_on(async {
        let timing = Timing {
            budget: Duration::from_millis(200),
            pause: Duration::from_millis(10),
            max_pause: Duration::from_millis(20),
            ..patient()
        };
        let (credentials, script) = scripted(&[], Step::Fail, timing);
        let Ok(Err(Error::Http(message))) =
            tokio::time::timeout(PATIENCE, credentials.token()).await
        else {
            panic!("expected an error");
        };
        let attempts = script.requests();
        assert!(attempts >= 2, "{attempts} attempts");
        let prefix = format!(
            "token from test endpoint: refused request {attempts} ({attempts} attempts in "
        );
        assert!(message.starts_with(&prefix), "{message}");
        // Until the pause after that is over, calls fail the same way, at once.
        let Err(Error::Http(again)) = credentials.token().await else {
            panic!("expected an error");
        };
        assert_eq!(again, message);
        assert_eq!(script.requests(), attempts);
    });
}

#[test]
fn a_failed_replacement_of_an_expired_token_is_one_request() {
    runtime().block_on(async {
        let (credentials, script) = scripted(&[], Step::Fail, patient());
        let now = Instant::now();
        seed(
            &credentials,
            Token {
                value: "old".into(),
                refresh_at: now,
                expires_at: now,
            },
        );
        let Err(Error::Http(message)) = credentials.token().await else {
            panic!("expected an error");
        };
        assert_eq!(message, "token from test endpoint: refused request 1");
        assert_eq!(script.requests(), 1);
    });
}

#[test]
fn a_refused_token_is_replaced_despite_a_pause() {
    runtime().block_on(async {
        let (credentials, script) = scripted(&[Step::Fail], Step::Token, patient());
        cache_due(&credentials, "old");
        assert_eq!(credentials.token().await.unwrap(), "old");
        until("the request to fail", || !requesting(&credentials)).await;
        // During the pause after that, the bucket refuses the valid token:
        // calls that held it share one request for another.
        let mut calls = JoinSet::new();
        for _ in 0..20 {
            let credentials = credentials.clone();
            calls.spawn(async move {
                assert!(credentials.forget("old"));
                credentials.token().await.unwrap()
            });
        }
        while let Some(token) = calls.join_next().await {
            assert_eq!(token.unwrap(), "t2");
        }
        assert_eq!(script.requests(), 2);
        // A second refusal soon after waits out a pause: no call retries.
        assert!(!credentials.forget("t2"));
        let Err(Error::Http(message)) = credentials.token().await else {
            panic!("expected an error");
        };
        assert_eq!(
            message,
            "token from test endpoint: the bucket refused two tokens within 3600 s"
        );
        assert_eq!(script.requests(), 2);
        // After the pause, one request.
        end_pause(&credentials);
        assert_eq!(credentials.token().await.unwrap(), "t3");
    });
}

#[test]
fn a_replacement_for_a_refused_token_is_one_request() {
    runtime().block_on(async {
        let (credentials, script) = scripted(&[Step::Token, Step::Fail], Step::Token, patient());
        assert_eq!(credentials.token().await.unwrap(), "t1");
        // Not a first token: no retries, but the usual pause after a failure.
        assert!(credentials.forget("t1"));
        let Err(Error::Http(message)) = credentials.token().await else {
            panic!("expected an error");
        };
        assert_eq!(message, "token from test endpoint: refused request 2");
        assert_eq!(script.requests(), 2);
        end_pause(&credentials);
        assert_eq!(credentials.token().await.unwrap(), "t3");
    });
}

#[test]
fn a_refused_token_is_forgotten_unless_replaced() {
    let (credentials, script) = scripted(&[], Step::Token, patient());
    let token = Token::new("current".into(), Instant::now(), Duration::from_mins(1));
    seed(&credentials, token);
    assert!(credentials.forget("older"));
    assert!(credentials.shared.lock().token.is_some());
    assert!(credentials.forget("current"));
    assert!(credentials.shared.lock().token.is_none());
    assert_eq!(script.requests(), 0);
}

#[test]
fn a_request_that_panics_fails_its_calls() {
    runtime().block_on(async {
        let (credentials, script) = scripted(&[Step::Panic], Step::Token, patient());
        let answers = tokens(&credentials, 5, PATIENCE).await;
        for answer in answers {
            let Err(Error::Http(message)) = answer else {
                panic!("expected an error");
            };
            assert_eq!(
                message,
                "token from test endpoint: the request ended without an answer"
            );
        }
        assert!(!requesting(&credentials));
        end_pause(&credentials);
        assert_eq!(credentials.token().await.unwrap(), "t2");
        assert_eq!(script.requests(), 2);
    });
}

#[test]
fn a_request_ends_only_itself() {
    runtime().block_on(async {
        let (credentials, script) = scripted(&[Step::Token], Step::Stall, patient());
        cache_due(&credentials, "t0");
        // Once the replacement is recorded, and before the request that got
        // it is done, the bucket refuses it and a call starts another.
        let hook: AfterRecord = Box::new(|shared| {
            let refused = Credentials {
                shared: shared.clone(),
            };
            assert!(refused.forget("t1"));
            assert!(shared.poll().is_none());
        });
        assert!(credentials.shared.after_record.set(hook).is_ok());
        let mut ended = credentials.shared.ended.subscribe();
        assert_eq!(credentials.token().await.unwrap(), "t0");
        tokio::time::timeout(PATIENCE, ended.changed())
            .await
            .unwrap()
            .unwrap();
        // The first request's end leaves the second under way, without an
        // error or a pause.
        assert_eq!(script.requests(), 2);
        assert!(requesting(&credentials));
        let state = credentials.shared.lock();
        let (error, retry_at, backoff) = (state.error.clone(), state.retry_at, state.backoff);
        drop(state);
        assert_eq!((error, retry_at, backoff), (None, None, Duration::ZERO));
    });
}

/// `status` and `body`, with `Metadata-Flavor: Google` as the metadata
/// server answers.
fn google(status: u16, body: &str) -> Answer {
    Answer::Reply {
        status,
        headers: vec![("metadata-flavor", "Google".into())],
        body: body.into(),
    }
}

/// The tokens of the metadata server at `server`, as on Google Cloud.
fn metadata(server: &Server, timing: Timing) -> Credentials {
    Credentials::over(metadata_source(&server.host), Http::new().unwrap(), timing)
}

/// Short limits, for tests that wait through them on a real socket.
fn quick() -> Timing {
    Timing {
        attempt: Duration::from_millis(300),
        budget: Duration::from_millis(800),
        pause: Duration::from_millis(50),
        max_pause: Duration::from_millis(100),
        ..patient()
    }
}

#[test]
fn a_dribbling_endpoint_is_given_up_on() {
    runtime().block_on(async {
        let endpoint = Server::serving(|_, _| Answer::Dribble).await;
        // Each 300 ms attempt would take over half an hour to read the whole answer, which
        // never pauses for as long as an attempt.
        let credentials = metadata(&endpoint, quick());
        let answers = tokens(&credentials, 5, PATIENCE).await;
        assert!(answers.iter().all(Result::is_err));
        // The same, for the replacement of a valid token.
        cache_due(&credentials, "old");
        end_pause(&credentials);
        let requests = endpoint.requests();
        assert_eq!(credentials.token().await.unwrap(), "old");
        until("the request to fail", || !requesting(&credentials)).await;
        assert_eq!(endpoint.requests(), requests + 1);
        let error = credentials.shared.lock().error.clone();
        assert_eq!(error.as_deref(), Some("no answer within 0.3 s"));
    });
}

#[test]
fn an_error_answer_names_its_status() {
    runtime().block_on(async {
        let endpoint = Server::serving(|_, _| google(403, r#"{"error": "nope"}"#)).await;
        let credentials = metadata(&endpoint, quick());
        let Err(Error::Http(message)) = credentials.token().await else {
            panic!("expected an error");
        };
        let prefix = format!(
            "token from metadata server http://{}/computeMetadata/v1/instance/service-accounts/\
             default/token: 403 nope (",
            endpoint.host
        );
        assert!(message.starts_with(&prefix), "{message}");
    });
}

#[test]
fn a_closed_connection_is_retried() {
    runtime().block_on(async {
        let endpoint = Server::serving(|_, _| Answer::Close).await;
        let timing = Timing {
            budget: Duration::from_millis(200),
            pause: Duration::from_millis(10),
            max_pause: Duration::from_millis(20),
            ..patient()
        };
        let credentials = metadata(&endpoint, timing);
        let Ok(Err(Error::Http(message))) =
            tokio::time::timeout(PATIENCE, credentials.token()).await
        else {
            panic!("expected an error");
        };
        assert!(!message.contains("no credentials found"), "{message}");
        assert!(message.contains(" attempts in "), "{message}");
        assert!(endpoint.requests() >= 2, "{message}");
    });
}
