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
use crate::fake_http::reply;

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

/// An environment of `vars`.
fn env(vars: &[(&str, &str)]) -> impl Fn(&str) -> Option<String> + use<> {
    let vars: Vec<(String, String)> = vars
        .iter()
        .map(|(name, value)| ((*name).to_owned(), (*value).to_owned()))
        .collect();
    move |name| vars.iter().find(|(n, _)| n == name).map(|(_, v)| v.clone())
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
        assert_eq!(check_url("token_uri", uri), Ok(()), "{uri}");
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
            check_url("token_uri", uri),
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
    let credentials = Credentials::new(parse(&file).unwrap(), Http::new().unwrap());
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

    let user = r#"{"type": "authorized_user", "client_id": "c", "client_secret": "s",
                   "refresh_token": "r"}"#;
    let credentials = Credentials::new(parse(user).unwrap(), Http::new().unwrap());
    assert_eq!(credentials.describe(), "authorized_user credentials");
    assert_eq!(credentials.quota_project(), None);
    let user = user.replace(r#""r""#, r#""r", "quota_project_id": "qp-1""#);
    let credentials = Credentials::new(parse(&user).unwrap(), Http::new().unwrap());
    assert_eq!(credentials.quota_project(), Some("qp-1"));

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
            r#"{"type": "authorized_user", "client_id": "c", "client_secret": "s"}"#.to_owned(),
            "authorized_user credentials have no refresh_token",
        ),
        (
            r#"{"type": "authorized_user", "client_id": "c", "client_secret": "s",
                "refresh_token": "r", "quota_project_id": "q\r\nx: y"}"#
                .to_owned(),
            "quota_project_id \"q\\r\\nx: y\" is not a project ID",
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
    let jwt = assertion(&key, "sa@p", Some("k1"), audience, SCOPE, 1_700_000_000).unwrap();
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

    // A token that impersonates another service account asks for every API.
    let jwt = assertion(&key, "sa@p", None, audience, CLOUD_PLATFORM, 1_700_000_000).unwrap();
    let parts: Vec<_> = jwt.split('.').collect();
    assert_eq!(
        decode(parts[0]),
        serde_json::json!({ "alg": "RS256", "typ": "JWT" })
    );
    let scope = "https://www.googleapis.com/auth/cloud-platform";
    assert_eq!(decode(parts[1])["scope"], scope);
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

/// The metadata server's token `t<n>`.
fn token(n: usize) -> Answer {
    let token = serde_json::json!({ "access_token": format!("t{n}"), "expires_in": 3600 });
    google(200, &token.to_string())
}

/// A metadata server that answers `script` in turn, then `token(n)` to its
/// `n`th request.
async fn tokens_after(script: Vec<Answer>) -> Server {
    Server::serving(move |_, n| script.get(n - 1).cloned().unwrap_or_else(|| token(n))).await
}

/// The tokens of the metadata server at `server`, as on Google Cloud.
fn metadata(server: &Server, timing: Timing) -> Credentials {
    Credentials::over(
        metadata_source(&server.host, true),
        Http::new().unwrap(),
        timing,
    )
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
fn signs_of_google_cloud() {
    assert!(!google_cloud(&env(&[]), None));
    assert!(!google_cloud(&env(&[]), Some("Virtual Machine\n")));
    assert!(!google_cloud(&env(&[("GCE_METADATA_HOST", "")]), None));
    assert!(google_cloud(&env(&[]), Some("Google Compute Engine\n")));
    assert!(google_cloud(&env(&[]), Some("Google")));
    assert!(google_cloud(&env(&[("GCE_METADATA_HOST", "h:1")]), None));
    assert!(google_cloud(
        &env(&[("KUBERNETES_SERVICE_HOST", "10.0.0.1")]),
        None
    ));
    // Cloud Run services, jobs and functions, and App Engine.
    assert!(google_cloud(&env(&[("K_SERVICE", "svc")]), None));
    assert!(google_cloud(&env(&[("CLOUD_RUN_JOB", "job")]), None));
    assert!(google_cloud(&env(&[("FUNCTION_TARGET", "main")]), None));
    assert!(google_cloud(&env(&[("FUNCTION_NAME", "f")]), None));
    assert!(google_cloud(&env(&[("GAE_ENV", "standard")]), None));
    assert!(google_cloud(&env(&[("GAE_SERVICE", "default")]), None));
    // Only a metadata server off Google Cloud is probed first.
    let Source::Metadata { probe, .. } = metadata_source("169.254.169.254", false) else {
        panic!("expected a metadata server");
    };
    assert_eq!(probe.as_deref(), Some("169.254.169.254:80"));
    for (host, address) in [
        ("h:8080", "h:8080"),
        ("[::1]", "[::1]:80"),
        ("[::1]:9", "[::1]:9"),
    ] {
        let Source::Metadata { probe, .. } = metadata_source(host, false) else {
            panic!("expected a metadata server");
        };
        assert_eq!(probe.as_deref(), Some(address));
    }
    let Source::Metadata { probe, .. } = metadata_source("169.254.169.254", true) else {
        panic!("expected a metadata server");
    };
    assert_eq!(probe, None);
}

#[test]
fn nothing_at_the_metadata_server_fails_at_once() {
    runtime().block_on(async {
        // Nothing listens on port 1. The pauses are an hour: a retry would
        // outlast the test.
        let source = metadata_source("127.0.0.1:1", false);
        let credentials = Credentials::over(source, Http::new().unwrap(), patient());
        let Ok(Err(Error::Http(message))) = tokio::time::timeout(PATIENCE, credentials.token()).await
        else {
            panic!("expected an error");
        };
        let prefix = "token from metadata server \
                      http://127.0.0.1:1/computeMetadata/v1/instance/service-accounts/default/token: \
                      no credentials found: nothing is at the metadata server 127.0.0.1:1 (";
        assert!(message.starts_with(prefix), "{message}");
        assert!(
            message.ends_with(
                "). Set GOOGLE_APPLICATION_CREDENTIALS to a credentials file, run `gcloud auth \
                 application-default login`, or run on Compute Engine or GKE"
            ),
            "{message}"
        );
    });
}

#[test]
fn a_metadata_server_that_is_there_is_retried() {
    runtime().block_on(async {
        // It accepts connections, then refuses requests in each way a new
        // GKE pod's might.
        let refused = |status| google(status, r#"{"error": "nope"}"#);
        let script = vec![refused(503), Answer::Close, google(429, ""), refused(403)];
        let endpoint = tokens_after(script).await;
        let source = metadata_source(&endpoint.host, false);
        let timing = Timing {
            attempt: Duration::from_secs(5),
            ..patient()
        };
        let credentials = Credentials::over(source, Http::new().unwrap(), timing);
        let token = tokio::time::timeout(PATIENCE, credentials.token()).await;
        assert_eq!(token.unwrap().unwrap(), "t5");
        let path = "/computeMetadata/v1/instance/service-accounts/default/token";
        let received = endpoint.received();
        assert_eq!(received.len(), 5);
        for request in received {
            assert_eq!(request.path, path);
            assert_eq!(request.header("metadata-flavor"), Some("Google"));
        }
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

#[test]
fn an_answer_that_is_not_googles_fails_at_once() {
    runtime().block_on(async {
        // Another cloud's metadata server, at the same address: on Google
        // Cloud or off it, its answers, a token among them, are not taken
        // and not retried.
        let token = r#"{"access_token":"x","expires_in":60}"#;
        for (status, body, on_google) in [
            (404, "<html>Not Found</html>", false),
            (401, "", true),
            (200, token, true),
        ] {
            let endpoint = Server::serving(move |_, _| reply(status, body)).await;
            let source = metadata_source(&endpoint.host, on_google);
            let credentials = Credentials::over(source, Http::new().unwrap(), patient());
            let Ok(Err(Error::Http(message))) =
                tokio::time::timeout(PATIENCE, credentials.token()).await
            else {
                panic!("expected an error for {status}");
            };
            let expected = format!(
                "token from metadata server http://{}/computeMetadata/v1/instance/\
                 service-accounts/default/token: no credentials found: the {status} answer is \
                 not from Google's metadata server, having no `Metadata-Flavor: Google` \
                 header. Set GOOGLE_APPLICATION_CREDENTIALS to a credentials file, run `gcloud \
                 auth application-default login`, or run on Compute Engine or GKE",
                endpoint.host
            );
            assert_eq!(message, expected);
            assert_eq!(endpoint.requests(), 1);
        }
    });
}

#[test]
fn a_headerless_answer_that_says_to_retry_is_retried() {
    runtime().block_on(async {
        // At the default address, a 429 or 5xx without the header may be a
        // proxy's or a load balancer's: it is retried as Google's are.
        let script = vec![reply(503, ""), reply(429, ""), reply(500, "")];
        let endpoint = tokens_after(script).await;
        let source = metadata_source(&endpoint.host, true);
        let timing = Timing {
            attempt: Duration::from_secs(5),
            ..patient()
        };
        let credentials = Credentials::over(source, Http::new().unwrap(), timing);
        let token = tokio::time::timeout(PATIENCE, credentials.token()).await;
        assert_eq!(token.unwrap().unwrap(), "t4");
        assert_eq!(endpoint.requests(), 4);
    });
}

#[test]
fn a_named_metadata_server_needs_no_header() {
    runtime().block_on(async {
        // GCE_METADATA_HOST names it: a proxy or an emulator that sends no
        // `Metadata-Flavor` is taken at its word, errors and tokens alike.
        let token = r#"{"access_token":"x","expires_in":60}"#;
        let endpoint =
            Server::serving(move |_, n| reply(if n == 1 { 404 } else { 200 }, token)).await;
        let source = metadata_server(Some(&endpoint.host), true);
        let timing = Timing {
            attempt: Duration::from_secs(5),
            ..patient()
        };
        let credentials = Credentials::over(source, Http::new().unwrap(), timing);
        let token = tokio::time::timeout(PATIENCE, credentials.token()).await;
        assert_eq!(token.unwrap().unwrap(), "x");
        assert_eq!(endpoint.requests(), 2);
        // Without a name, the default address requires the header.
        let Source::Metadata { flavor, .. } = metadata_server(None, true) else {
            panic!("expected a metadata server");
        };
        assert!(flavor);
    });
}

#[test]
fn a_replacement_that_is_not_googles_fails() {
    runtime().block_on(async {
        let endpoint = Server::serving(|_, _| reply(404, "")).await;
        let credentials = metadata(&endpoint, patient());
        cache_due(&credentials, "old");
        assert_eq!(credentials.token().await.unwrap(), "old");
        until("the request to fail", || !requesting(&credentials)).await;
        let error = credentials.shared.lock().error.clone();
        assert_eq!(
            error.as_deref(),
            Some(
                "the 404 answer is not from Google's metadata server, having no \
                 `Metadata-Flavor: Google` header"
            )
        );
        assert_eq!(endpoint.requests(), 1);
    });
}

#[test]
fn where_gcloud_keeps_its_credentials() {
    let file = |vars, windows| gcloud_credentials_file(&env(vars), windows);
    let home = [
        ("HOME", "/home/u"),
        ("APPDATA", "C:\\Users\\u\\AppData\\Roaming"),
    ];
    assert_eq!(
        file(&home, false),
        Some(PathBuf::from(
            "/home/u/.config/gcloud/application_default_credentials.json"
        ))
    );
    assert_eq!(
        file(&home, true),
        Some(
            PathBuf::from("C:\\Users\\u\\AppData\\Roaming")
                .join("gcloud")
                .join("application_default_credentials.json")
        )
    );
    let config = [("HOME", "/home/u"), ("CLOUDSDK_CONFIG", "/etc/gcloud")];
    let expected = Some(PathBuf::from(
        "/etc/gcloud/application_default_credentials.json",
    ));
    assert_eq!(file(&config, false), expected);
    assert_eq!(file(&config, true), expected);
    assert_eq!(file(&[("HOME", ""), ("CLOUDSDK_CONFIG", "")], false), None);
    assert_eq!(file(&[], true), None);
}

#[test]
fn user_credentials_exchange_their_refresh_token() {
    runtime().block_on(async {
        let endpoint = tokens_after(Vec::new()).await;
        // Google's refresh tokens start with `1//`.
        let file = format!(
            r#"{{"type": "authorized_user", "client_id": "c 1", "client_secret": "s+ /=",
                 "refresh_token": "1//r+x=", "token_uri": "http://{}/token"}}"#,
            endpoint.host
        );
        let credentials = Credentials::new(parse(&file).unwrap(), Http::new().unwrap());
        assert_eq!(credentials.token().await.unwrap(), "t1");
        let received = endpoint.received();
        assert_eq!(received.len(), 1);
        assert_eq!(received[0].path, "/token");
        assert_eq!(
            received[0].header("content-type"),
            Some("application/x-www-form-urlencoded")
        );
        assert_eq!(
            String::from_utf8(received[0].body.clone()).unwrap(),
            "grant_type=refresh_token&client_id=c%201&client_secret=s%2B%20%2F%3D&\
             refresh_token=1%2F%2Fr%2Bx%3D"
        );
    });
}

const USER: &str = r#"{"type": "authorized_user", "client_id": "c", "client_secret": "s",
    "refresh_token": "r"}"#;
const TARGET: &str = "t@p.iam.gserviceaccount.com";

/// An `impersonated_service_account` file of `fields`.
fn impersonated_file(fields: &str) -> String {
    format!(r#"{{"type": "impersonated_service_account" {fields}}}"#)
}

/// `generateAccessToken` for `TARGET` at `base`.
fn iam_url(base: &str) -> String {
    format!("{base}/v1/projects/-/serviceAccounts/{TARGET}:generateAccessToken")
}

#[test]
fn impersonated_credentials_files() {
    let url = iam_url("https://iamcredentials.googleapis.com");
    let fields = format!(
        r#", "service_account_impersonation_url": "{url}", "source_credentials": {USER},
           "delegates": ["d1@p.iam.gserviceaccount.com",
                         "projects/-/serviceAccounts/d2@p.iam.gserviceaccount.com"]"#
    );
    let file = impersonated_file(&fields);
    let source = parse(&file).unwrap();
    assert_eq!(
        describe(&source),
        format!("service account {TARGET}, impersonated with authorized_user credentials")
    );
    let Source::Impersonated {
        source,
        target,
        delegates,
        url: parsed,
        quota_project,
    } = source
    else {
        panic!("expected impersonation");
    };
    assert!(matches!(*source, Source::AuthorizedUser { .. }));
    assert_eq!((target.as_str(), parsed), (TARGET, url.clone()));
    assert_eq!(quota_project, None);
    assert_eq!(
        delegates,
        [
            "projects/-/serviceAccounts/d1@p.iam.gserviceaccount.com",
            "projects/-/serviceAccounts/d2@p.iam.gserviceaccount.com"
        ]
    );

    // A service account's key as the source asks for every API.
    let key = service_account("");
    let file = impersonated_file(&format!(
        r#", "service_account_impersonation_url": "{url}", "source_credentials": {key}"#
    ));
    let Source::Impersonated {
        source, delegates, ..
    } = parse(&file).unwrap()
    else {
        panic!("expected impersonation");
    };
    assert!(delegates.is_empty());
    let Source::ServiceAccount { scope, .. } = *source else {
        panic!("expected a service account");
    };
    assert_eq!(scope, CLOUD_PLATFORM);
    // Not as a credentials file of its own.
    let Source::ServiceAccount { scope, .. } = parse(&key).unwrap() else {
        panic!("expected a service account");
    };
    assert_eq!(scope, SCOPE);
}

/// The target and URL of an `impersonated_service_account` file whose
/// `service_account_impersonation_url` is `url`.
fn impersonation_of(url: &str) -> std::result::Result<(String, String), String> {
    let file = impersonated_file(&format!(
        r#", "service_account_impersonation_url": "{url}", "source_credentials": {USER}"#
    ));
    let Source::Impersonated { target, url, .. } = parse(&file)? else {
        panic!("expected impersonation");
    };
    Ok((target, url))
}

#[test]
fn only_the_account_of_an_impersonation_url_is_taken() {
    let iam = iam_url("https://iamcredentials.googleapis.com");
    // Another host, lookalikes among them, is not where the source token
    // goes: IAM is.
    for host in [
        "https://iamcredentials.googleapis.com:443",
        "https://IAMCREDENTIALS.googleapis.com",
        "https://iamcredentials.googleapis.com.attacker.example",
        "https://attacker.example",
        "https://localhost.attacker.example",
        "https://127.0.0.1.attacker.example",
        "https://10.0.0.1",
    ] {
        let expected = (TARGET.to_owned(), iam.clone());
        assert_eq!(impersonation_of(&iam_url(host)), Ok(expected), "{host}");
    }
    // A loopback one is, for an emulator or a test.
    for host in [
        "http://127.0.0.1:8080",
        "http://localhost:1",
        "https://[::1]:9",
    ] {
        let url = iam_url(host);
        assert_eq!(impersonation_of(&url), Ok((TARGET.to_owned(), url.clone())));
    }
    // A unique ID.
    let base = "https://iamcredentials.googleapis.com/v1/projects/-/serviceAccounts/";
    let id = format!("{base}1234:generateAccessToken");
    assert_eq!(impersonation_of(&id), Ok(("1234".to_owned(), id.clone())));
    // Any other path, or a query, is refused.
    for url in [
        format!("{iam}?x=1"),
        format!("{iam}#x"),
        format!("https://attacker.example/iamcredentials.googleapis.com{}", iam_url("")),
        format!("https://iamcredentials.googleapis.com/{}", iam_url("")),
        iam_url("https://iamcredentials.googleapis.com/x"),
        "https://iamcredentials.googleapis.com/v1/projects/p/serviceAccounts/t@p:generateAccessToken"
            .to_owned(),
        format!("{base}t@p:generateIdToken"),
        format!("{base}t@p:generateAccessToken/x"),
        format!("{base}t@p/x:generateAccessToken"),
        format!("{base}:generateAccessToken"),
        format!("{base}@:generateAccessToken"),
        format!("{base}t@p@q:generateAccessToken"),
        format!("{base}t%40p:generateAccessToken"),
    ] {
        assert!(impersonation_of(&url).is_err(), "{url}");
    }
}

#[test]
fn impersonated_credentials_files_that_are_refused() {
    let url = iam_url("https://iamcredentials.googleapis.com");
    let with_url = |url: &str| {
        impersonated_file(&format!(
            r#", "service_account_impersonation_url": "{url}", "source_credentials": {USER}"#
        ))
    };
    let cleartext = iam_url("http://iamcredentials.googleapis.com");
    let userinfo = iam_url("http://localhost@attacker.example");
    for (file, error) in [
        (
            impersonated_file(&format!(r#", "source_credentials": {USER}"#)),
            "impersonated_service_account credentials have no service_account_impersonation_url"
                .to_owned(),
        ),
        (
            with_url(&cleartext),
            format!(
                "service_account_impersonation_url {cleartext:?} must be https, or http on a \
                 loopback address"
            ),
        ),
        (
            with_url(&userinfo),
            format!(
                "service_account_impersonation_url {userinfo:?} must not hold a user name or password"
            ),
        ),
        (
            with_url("https://iamcredentials.googleapis.com/v1/token"),
            "service_account_impersonation_url \"https://iamcredentials.googleapis.com/v1/token\" \
             is not a service account's generateAccessToken URL"
                .to_owned(),
        ),
        (
            impersonated_file(&format!(
                r#", "service_account_impersonation_url": "{url}""#
            )),
            "impersonated_service_account credentials have no source_credentials".to_owned(),
        ),
        (
            with_url(&url).replace(
                r#""source_credentials": {"type": "authorized_user""#,
                r#""source_credentials": {"type": "impersonated_service_account""#,
            ),
            "source_credentials of type \"impersonated_service_account\" are not supported"
                .to_owned(),
        ),
        (
            with_url(&url).replace(
                r#""source_credentials": {"type": "authorized_user""#,
                r#""source_credentials": {"type": "external_account""#,
            ),
            "source_credentials: credentials of type \"external_account\" are not supported: use \
             a service account key, authorized_user or impersonated_service_account \
             credentials, or the metadata server"
                .to_owned(),
        ),
        (
            impersonated_file(&format!(
                r#", "service_account_impersonation_url": "{url}", "source_credentials": {USER},
                   "delegates": ["d1/x"]"#
            )),
            "\"d1/x\" is not a service account's email address or unique ID".to_owned(),
        ),
        (
            with_url(&url).replace(
                r#""refresh_token": "r""#,
                r#""refresh_token": "r", "quota_project_id": "q\r\nx: y""#,
            ),
            "source_credentials: quota_project_id \"q\\r\\nx: y\" is not a project ID".to_owned(),
        ),
        (
            impersonated_file(&format!(
                r#", "service_account_impersonation_url": "{url}", "source_credentials": {USER},
                   "quota_project_id": "Q""#
            )),
            "quota_project_id \"Q\" is not a project ID".to_owned(),
        ),
    ] {
        let Err(message) = parse(&file) else {
            panic!("{file} was accepted");
        };
        assert_eq!(message, error);
    }
}

#[test]
fn impersonation_from_the_environment() {
    let metadata = || metadata_source("169.254.169.254", true);
    let Source::Impersonated {
        source,
        target,
        delegates,
        url,
        quota_project,
    } = impersonate(metadata(), TARGET).unwrap()
    else {
        panic!("expected impersonation");
    };
    assert!(matches!(*source, Source::Metadata { .. }));
    assert_eq!(target, TARGET);
    assert!(delegates.is_empty());
    assert_eq!(quota_project, None);
    assert_eq!(url, iam_url("https://iamcredentials.googleapis.com"));

    // gcloud's delegation chain: each impersonates the next, the last the
    // target.
    let chain = format!("a@p.iam.gserviceaccount.com, b@p.iam.gserviceaccount.com ,{TARGET}");
    let Source::Impersonated {
        target, delegates, ..
    } = impersonate(metadata(), &chain).unwrap()
    else {
        panic!("expected impersonation");
    };
    assert_eq!(target, TARGET);
    assert_eq!(
        delegates,
        [
            "projects/-/serviceAccounts/a@p.iam.gserviceaccount.com",
            "projects/-/serviceAccounts/b@p.iam.gserviceaccount.com"
        ]
    );

    // A service account's key as the source asks for every API.
    let Source::Impersonated { source, .. } =
        impersonate(parse(&service_account("")).unwrap(), TARGET).unwrap()
    else {
        panic!("expected impersonation");
    };
    let Source::ServiceAccount { scope, .. } = *source else {
        panic!("expected a service account");
    };
    assert_eq!(scope, CLOUD_PLATFORM);

    let already = impersonate(metadata(), TARGET).unwrap();
    let Err(message) = impersonate(already, "u@p.iam.gserviceaccount.com") else {
        panic!("impersonating twice was accepted");
    };
    assert_eq!(
        message,
        format!("the credentials impersonate {TARGET} already")
    );
    // A unique ID, as a target or a delegate.
    let chain = "projects/-/serviceAccounts/104,103";
    let Source::Impersonated {
        target, delegates, ..
    } = impersonate(metadata(), chain).unwrap()
    else {
        panic!("expected impersonation");
    };
    assert_eq!(target, "103");
    assert_eq!(delegates, ["projects/-/serviceAccounts/104"]);
    for chain in [
        "",
        "t@p,",
        "t",
        "@",
        "t@",
        "@p",
        "t@p@q",
        "1 2",
        "-1",
        "t@p/x",
        "t@p?x",
        "t@p:generateAccessToken",
        "t@p%2fx",
    ] {
        assert!(
            impersonate(metadata(), chain).is_err(),
            "{chain:?} was accepted"
        );
    }
}

#[test]
fn rfc3339_times() {
    let ms = |text: &str| parse_rfc3339_ms(text);
    assert_eq!(ms("1970-01-01T00:00:00Z"), Some(0));
    assert_eq!(ms("2099-12-31T23:59:59.999Z"), Some(4_102_444_799_999));
    assert_eq!(ms("2026-11-10T01:02:03Z"), Some(1_794_272_523_000));
    assert_eq!(ms("2026-11-10t01:02:03z"), Some(1_794_272_523_000));
    assert_eq!(ms("2026-11-10T01:02:03.5Z"), Some(1_794_272_523_500));
    assert_eq!(
        ms("2026-11-10T01:02:03.123456789Z"),
        Some(1_794_272_523_123)
    );
    assert_eq!(ms("2026-11-10T02:02:03+01:00"), Some(1_794_272_523_000));
    assert_eq!(ms("2026-11-10T00:32:03-00:30"), Some(1_794_272_523_000));
    // Leap days.
    assert_eq!(ms("2028-02-29T00:00:00Z"), Some(1_835_395_200_000));
    assert_eq!(ms("2000-02-29T00:00:00Z"), Some(951_782_400_000));
    assert_eq!(ms("2026-12-31T00:00:00Z"), Some(1_798_675_200_000));
    // A leap second, as at the end of a day.
    assert_eq!(ms("2016-12-31T23:59:60Z"), Some(1_483_228_800_000));
    for text in [
        "",
        "2026-11-10",
        "2026-11-10T01:02:03",
        "2026-11-10T01:02:03.Z",
        "2026-11-10T01:02:03+0100",
        "2026-11-10T01:02:03+01",
        "2026-11-10T01:02:03+-1:00",
        "2026-11-10T01:02:03++1:00",
        "2026-11-10T01:02:03+01:-1",
        "2026-11-10T01:02:03+01:+1",
        "2026-11-10T01:02:03+0a:00",
        "2026-02-31T01:02:03Z",
        "2026-02-29T01:02:03Z",
        "2100-02-29T01:02:03Z",
        "2026-04-31T01:02:03Z",
        "2026-11-00T01:02:03Z",
        "2026-13-10T01:02:03Z",
        "2026-11-10T24:02:03Z",
        "2026-11-10T01:60:03Z",
        "2026-11-10T12:00:60Z",
        "2026-11-10T23:58:60Z",
        "2026-11-10T22:59:60Z",
        "2026-11-10T23:59:61Z",
        "2026-11-10 01:02:03Z",
        "x026-11-10T01:02:03Z",
        "2026-11-10X01:02:03Z",
        "2026-11-10T01:02:03Zjunk",
        "+026-11-10T01:02:03Z",
    ] {
        assert_eq!(ms(text), None, "{text}");
    }
}

#[test]
fn impersonated_answers() {
    let wall = UNIX_EPOCH + Duration::from_secs(1_700_000_000);
    let now = Instant::now();
    let answer = |token: &str, expires: &str| {
        let body = serde_json::json!({ "accessToken": token, "expireTime": expires });
        parse_impersonated(body.to_string().as_bytes(), None, wall, now)
    };
    let in_half_an_hour = "2023-11-14T22:43:20Z";
    let token = answer("ya29.i", in_half_an_hour).unwrap();
    assert_eq!(token.value, "ya29.i");
    assert_eq!(token.expires_at - now, Duration::from_mins(30));
    assert_eq!(token.refresh_at - now, Duration::from_mins(25));
    // However long it claims to last, it lasts at most the hour asked for.
    let token = answer("ya29.i", "2999-01-01T00:00:00Z").unwrap();
    assert_eq!(token.expires_at - now, Duration::from_hours(1));
    let expired = "2023-11-14T22:13:20Z";
    assert_eq!(
        answer("ya29.i", expired).unwrap_err(),
        format!("the answer's token has expired (expireTime {expired})")
    );
    assert_eq!(
        answer("ya29.i", "soon").unwrap_err(),
        "the answer's expireTime \"soon\" is not a time"
    );
    assert_eq!(
        answer("", in_half_an_hour).unwrap_err(),
        "the answer has an empty accessToken"
    );
    assert!(parse_impersonated(b"{}", None, wall, now).is_err());
}

#[test]
fn an_impersonated_tokens_lifetime_is_on_iams_clock() {
    // IAM's clock says 1_700_000_000 s: it answers a token for an hour.
    let iam_ms = 1_700_000_000_000;
    let body = serde_json::json!({
        "accessToken": "ya29.i",
        "expireTime": "2023-11-14T23:13:20Z",
    });
    let body = body.to_string();
    let now = Instant::now();
    let lifetime = |date: Option<i64>, wall_ms: i64| {
        let wall = UNIX_EPOCH + Duration::from_millis(wall_ms.try_into().unwrap());
        let token = parse_impersonated(body.as_bytes(), date, wall, now);
        token.map(|t| t.expires_at - now)
    };
    let hour = Duration::from_hours(1);
    // With the answer's Date, the hour, whatever this machine's clock says.
    for skew_ms in [0, 3_600_000, -3_600_000, 7_200_000] {
        assert_eq!(
            lifetime(Some(iam_ms), iam_ms + skew_ms),
            Ok(hour),
            "{skew_ms}"
        );
    }
    // Without one, this machine's clock: an hour fast, the token seems to
    // have expired; slow, it lasts no longer than the hour asked for.
    assert!(lifetime(None, iam_ms + 3_600_000).is_err());
    assert_eq!(lifetime(None, iam_ms - 3_600_000), Ok(hour));
    assert_eq!(
        lifetime(None, iam_ms + 600_000),
        Ok(Duration::from_mins(50))
    );
}

/// A token endpoint for `authorized_user` credentials and IAM's
/// `generateAccessToken` for `TARGET`, on a real socket: they answer
/// `source` and `impersonation`, or tokens when those are `None`. IAM's
/// token expires half an hour after its answer's `Date`, which is years
/// behind this machine's clock.
async fn iam(source: Option<Answer>, impersonation: Option<Answer>) -> (Server, String) {
    let server = Server::serving(move |request, _| {
        if request.path == "/token" {
            let token = serde_json::json!({ "access_token": "source", "expires_in": 3600 });
            return source
                .clone()
                .unwrap_or_else(|| reply(200, &token.to_string()));
        }
        assert_eq!(request.path, iam_url(""));
        if request.header("authorization") != Some("Bearer source") {
            return reply(401, "");
        }
        let token = serde_json::json!({
            "accessToken": "impersonated",
            "expireTime": "2023-11-14T22:43:20Z",
        });
        impersonation.clone().unwrap_or_else(|| Answer::Reply {
            status: 200,
            headers: vec![("date", "Tue, 14 Nov 2023 22:13:20 GMT".into())],
            body: token.to_string(),
        })
    })
    .await;
    let base = format!("http://{}", server.host);
    let user = USER.replace(
        r#""refresh_token": "r""#,
        &format!(
            r#""refresh_token": "r", "token_uri": "{base}/token", "quota_project_id": "qp-1""#
        ),
    );
    let file = impersonated_file(&format!(
        r#", "service_account_impersonation_url": "{}", "source_credentials": {user},
           "delegates": ["d@p.iam.gserviceaccount.com"]"#,
        iam_url(&base)
    ));
    (server, file)
}

/// Short pauses, for first tokens that fail on a real socket.
fn failing() -> Timing {
    Timing {
        attempt: Duration::from_secs(5),
        budget: Duration::from_millis(100),
        pause: Duration::from_millis(10),
        max_pause: Duration::from_millis(20),
        ..patient()
    }
}

#[test]
fn an_impersonated_token_is_exchanged_for_a_source_token() {
    runtime().block_on(async {
        let (server, file) = iam(None, None).await;
        let credentials = Credentials::over(parse(&file).unwrap(), Http::new().unwrap(), failing());
        let token = tokio::time::timeout(PATIENCE, credentials.token()).await;
        assert_eq!(token.unwrap().unwrap(), "impersonated");
        // The token lasts as long as its expireTime says by IAM's clock,
        // within the time the requests took.
        let left = credentials.shared.lock().token.as_ref().unwrap().expires_at - Instant::now();
        assert!(left <= Duration::from_mins(30), "{left:?}");
        assert!(left > Duration::from_mins(29), "{left:?}");

        let received = server.received();
        assert_eq!(received.len(), 2);
        let (source, generate) = (&received[0], &received[1]);
        assert_eq!(source.method, "POST");
        assert_eq!(source.header("x-goog-user-project"), None);
        // The source's quota project is billed for the call.
        assert_eq!(generate.header("x-goog-user-project"), Some("qp-1"));
        let form = String::from_utf8(source.body.clone()).unwrap();
        assert!(form.starts_with("grant_type=refresh_token&"), "{form}");
        assert_eq!(generate.method, "POST");
        assert_eq!(generate.header("content-type"), Some("application/json"));
        let body: serde_json::Value = serde_json::from_slice(&generate.body).unwrap();
        assert_eq!(
            body,
            serde_json::json!({
                "scope": ["https://www.googleapis.com/auth/devstorage.read_write"],
                "lifetime": "3600s",
                "delegates": ["projects/-/serviceAccounts/d@p.iam.gserviceaccount.com"],
            })
        );
    });
}

#[test]
fn a_service_account_key_bills_no_quota_project() {
    runtime().block_on(async {
        let (server, _) = iam(None, None).await;
        let base = format!("http://{}", server.host);
        let key = service_account(&format!(
            r#", "token_uri": "{base}/token", "quota_project_id": "qp-2""#
        ));
        let file = impersonated_file(&format!(
            r#", "service_account_impersonation_url": "{}", "source_credentials": {key},
               "quota_project_id": "qp-3""#,
            iam_url(&base)
        ));
        let credentials = Credentials::over(parse(&file).unwrap(), Http::new().unwrap(), failing());
        // The file's quota project is for the bucket's requests.
        assert_eq!(credentials.quota_project(), Some("qp-3"));
        let token = tokio::time::timeout(PATIENCE, credentials.token()).await;
        assert_eq!(token.unwrap().unwrap(), "impersonated");
        let received = server.received();
        assert_eq!(received.len(), 2);
        assert!(
            received
                .iter()
                .all(|r| r.header("x-goog-user-project").is_none())
        );
    });
}

#[test]
fn impersonation_errors_name_their_step() {
    runtime().block_on(async {
        let invalid = r#"{"error": "invalid_grant", "error_description": "Bad Request"}"#;
        let denied = r#"{"error": {"code": 403, "status": "PERMISSION_DENIED",
            "message": "Permission 'iam.serviceAccounts.getAccessToken' denied"}}"#;
        let expired = serde_json::json!({
            "accessToken": "impersonated",
            "expireTime": "2000-01-01T00:00:00Z",
        });
        let prefix = format!(
            "token from service account {TARGET}, impersonated with authorized_user credentials: "
        );
        for (source, impersonation, step) in [
            (
                Some(reply(400, invalid)),
                None,
                "source token: 400 invalid_grant: Bad Request (",
            ),
            (
                None,
                Some(reply(403, denied)),
                "impersonation: 403 PERMISSION_DENIED: Permission \
                 'iam.serviceAccounts.getAccessToken' denied (",
            ),
            (
                None,
                Some(reply(200, &expired.to_string())),
                "impersonation: the answer's token has expired (expireTime \
                 2000-01-01T00:00:00Z) (",
            ),
        ] {
            let (_server, file) = iam(source, impersonation).await;
            let credentials =
                Credentials::over(parse(&file).unwrap(), Http::new().unwrap(), failing());
            let answer = tokio::time::timeout(PATIENCE, credentials.token()).await;
            let Ok(Err(Error::Http(message))) = answer else {
                panic!("expected an error");
            };
            assert!(message.starts_with(&format!("{prefix}{step}")), "{message}");
        }
    });
}

#[test]
fn impersonation_without_a_metadata_server_fails_at_once() {
    runtime().block_on(async {
        let source = impersonate(metadata_source("127.0.0.1:1", false), TARGET).unwrap();
        let credentials = Credentials::over(source, Http::new().unwrap(), patient());
        let answer = tokio::time::timeout(PATIENCE, credentials.token()).await;
        let Ok(Err(Error::Http(message))) = answer else {
            panic!("expected an error");
        };
        let prefix = format!(
            "token from service account {TARGET}, impersonated with metadata server \
             http://127.0.0.1:1/computeMetadata/v1/instance/service-accounts/default/token: \
             source token: no credentials found: "
        );
        assert!(message.starts_with(&prefix), "{message}");
    });
}
