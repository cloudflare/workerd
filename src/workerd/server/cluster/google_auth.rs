// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! OAuth 2.0 access tokens for Cloud Storage, from the service account key
//! `GOOGLE_APPLICATION_CREDENTIALS` names, else from the metadata server of
//! the Compute Engine VM or GKE pod the process runs on. GKE's Workload
//! Identity serves the token of the pod's Kubernetes service account there.
//!
//! A service account key signs a JWT that its token endpoint exchanges for a
//! token (the JWT bearer grant, RFC 7523). Other kinds of credentials file
//! are refused. A key's token endpoint must be `https`, or on a loopback
//! address.
//!
//! A bucket call never waits for a token while the cached one is valid: once
//! the token is due for replacement, a call starts one request for a new one
//! in the background, a task of its own that outlives the call, and goes on
//! with the current token. A request has 10 s for all of it, connecting and
//! the whole answer. A failed request is retried after a pause of 10 s,
//! doubling up to 30 s, and logged once per attempt. Only calls with no valid
//! token wait, all for the same request. Before the first token, whatever
//! the source, that request is retried: attempts start for up to 30 s and
//! each has its 10 s, so a first token is given up on within about 40 s. The
//! metadata server of a new GKE pod may refuse requests for its first
//! seconds.
//!
//! A token the bucket refuses (401) is dropped, and the next call requests
//! another, once, even during a pause after a failed request; a failure is
//! not retried but starts the usual pause. When a second token is refused
//! within 10 s of that, bucket calls wait for the pause instead.

use std::path::Path;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::PoisonError;
use std::time::Duration;
use std::time::Instant;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use base64::Engine;
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use ring::rand::SystemRandom;
use ring::signature::RSA_PKCS1_SHA256;
use ring::signature::RsaKeyPair;
use rustls::pki_types::PrivatePkcs8KeyDer;
use rustls::pki_types::pem::PemObject;
use serde::Deserialize;

use crate::Error;
use crate::Result;
use crate::client::encode;
use crate::http::Http;
use crate::http::Payload;

/// The metadata server's address on Compute Engine and GKE, unless
/// `GCE_METADATA_HOST` names another.
pub const METADATA_HOST: &str = "169.254.169.254";
/// Where a key without a `token_uri` exchanges its JWT.
const TOKEN_URI: &str = "https://oauth2.googleapis.com/token";
/// Reading, writing, listing and deleting objects.
const SCOPE: &str = "https://www.googleapis.com/auth/devstorage.read_write";
/// The lifetime a service account's JWT asks for, the most Google allows.
const JWT_LIFETIME_S: u64 = 3600;
/// How long before its expiry a token is replaced, at most: never later than
/// halfway through its lifetime.
const REFRESH_MARGIN: Duration = Duration::from_mins(5);
/// The longest lifetime a token is taken to have, whatever it claims.
const MAX_LIFETIME: Duration = Duration::from_hours(24);
/// The most of an error answer's text that goes into a message.
const MAX_ERROR_TEXT: usize = 200;

/// How long token requests and the pauses between them take.
#[derive(Clone, Copy, Debug)]
struct Timing {
    /// One request.
    attempt: Duration,
    /// How long requests for a first token start, pauses included.
    budget: Duration,
    /// The first pause between those requests, doubling up to `max_pause`.
    pause: Duration,
    max_pause: Duration,
    /// The pause after a failed request before the next one may start,
    /// doubling up to `max_backoff`.
    backoff: Duration,
    max_backoff: Duration,
}

impl Default for Timing {
    fn default() -> Self {
        Self {
            attempt: Duration::from_secs(10),
            budget: Duration::from_secs(30),
            pause: Duration::from_millis(500),
            max_pause: Duration::from_secs(4),
            backoff: Duration::from_secs(10),
            max_backoff: Duration::from_secs(30),
        }
    }
}

/// Where tokens come from, and the one in use. Clones share both.
#[derive(Clone)]
pub struct Credentials {
    shared: Arc<Shared>,
}

struct Shared {
    source: Source,
    http: Http,
    timing: Timing,
    state: Mutex<State>,
    /// Counts the requests that ended, so that waiting calls wake.
    ended: tokio::sync::watch::Sender<u64>,
    #[cfg(test)]
    after_record: std::sync::OnceLock<AfterRecord>,
}

/// Runs after a request recorded its answer, before its guard is dropped.
#[cfg(test)]
type AfterRecord = Box<dyn Fn(&Arc<Shared>) + Send + Sync>;

#[derive(Default)]
struct State {
    token: Option<Token>,
    /// A request for a token is under way.
    requesting: bool,
    /// A request answered a token once: a request after that, when there is
    /// none, replaces one rather than getting a first.
    had_token: bool,
    /// After a failed request, none starts before this.
    retry_at: Option<Instant>,
    /// The pause after the last failed request.
    backoff: Duration,
    /// What the last failed request answered.
    error: Option<String>,
    /// When a refused token last cut a pause short.
    forced_at: Option<Instant>,
}

enum Source {
    /// A service account's key: it signs a JWT that `token_uri` exchanges.
    ServiceAccount {
        email: String,
        key_id: Option<String>,
        key: Box<RsaKeyPair>,
        token_uri: String,
    },
    /// The metadata server's token endpoint.
    Metadata { url: String },
    /// Answers in place of a token endpoint.
    #[cfg(test)]
    Test(TestEndpoint),
}

#[cfg(test)]
type TestEndpoint = Arc<
    dyn Fn() -> futures::future::BoxFuture<'static, std::result::Result<Token, String>>
        + Send
        + Sync,
>;

/// Ends its request when dropped, as failed unless it recorded an answer
/// (`recorded`), and wakes the calls waiting for it. Once the answer is
/// recorded, another request may have started: that one is left be.
struct Ending {
    shared: Arc<Shared>,
    recorded: bool,
}

impl Drop for Ending {
    fn drop(&mut self) {
        let shared = &self.shared;
        if !self.recorded {
            let error = "the request ended without an answer".to_owned();
            shared.record(&mut shared.lock(), Err(error));
        }
        shared.ended.send_modify(|n| *n += 1);
    }
}

/// An access token and when it is due to be replaced.
#[derive(Clone)]
pub struct Token {
    pub value: String,
    pub refresh_at: Instant,
    pub expires_at: Instant,
}

impl std::fmt::Debug for Token {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Token")
            .field("value", &"<redacted>")
            .field("refresh_at", &self.refresh_at)
            .field("expires_at", &self.expires_at)
            .finish()
    }
}

impl Token {
    /// A token answered at `now` that is valid for `lifetime`, at most
    /// `MAX_LIFETIME`.
    pub fn new(value: String, now: Instant, lifetime: Duration) -> Self {
        let lifetime = lifetime.min(MAX_LIFETIME);
        let early = lifetime.saturating_sub(REFRESH_MARGIN).max(lifetime / 2);
        Self {
            value,
            refresh_at: now + early,
            expires_at: now + lifetime,
        }
    }
}

/// The fields of a credentials file this reads.
#[derive(Deserialize)]
struct File {
    #[serde(rename = "type")]
    kind: String,
    client_email: Option<String>,
    private_key: Option<String>,
    private_key_id: Option<String>,
    token_uri: Option<String>,
}

/// A token endpoint's answer.
#[derive(Deserialize)]
struct Answer {
    access_token: String,
    expires_in: u64,
}

impl Credentials {
    /// The credentials file at `path`, whose tokens come over `http`.
    pub fn from_file(path: &Path, http: Http) -> Result<Self> {
        let shown = path.display();
        let text =
            std::fs::read_to_string(path).map_err(|e| Error::Config(format!("{shown}: {e}")))?;
        Self::from_json(&text, http).map_err(|e| Error::Config(format!("{shown}: {e}")))
    }

    /// A credentials file's contents.
    pub fn from_json(text: &str, http: Http) -> std::result::Result<Self, String> {
        Ok(Self::over(parse_file(text)?, http, Timing::default()))
    }

    /// The metadata server at `host`, `host[:port]`.
    pub fn metadata(host: &str, http: Http) -> Self {
        Self::over(metadata_source(host), http, Timing::default())
    }

    fn over(source: Source, http: Http, timing: Timing) -> Self {
        Self {
            shared: Arc::new(Shared {
                source,
                http,
                timing,
                state: Mutex::default(),
                ended: tokio::sync::watch::Sender::new(0),
                #[cfg(test)]
                after_record: std::sync::OnceLock::new(),
            }),
        }
    }

    /// A valid access token. While the cached one is valid, at once; else
    /// once the request under way, which this may start, has ended.
    pub async fn token(&self) -> Result<String> {
        let mut ended = self.shared.ended.subscribe();
        loop {
            if let Some(answer) = self.shared.poll() {
                return answer.map_err(|e| self.shared.error(&e));
            }
            if ended.changed().await.is_err() {
                return Err(self.shared.error("the token request was dropped"));
            }
        }
    }

    /// Drops `token`, which the bucket refused, unless it was replaced
    /// already, and lets a request for another start at once, unless one
    /// did for a refusal within the last `timing.backoff`. Answers whether
    /// to retry the call: whether `token` can have a replacement without a
    /// pause.
    pub fn forget(&self, token: &str) -> bool {
        let backoff = self.shared.timing.backoff;
        let now = Instant::now();
        let mut state = self.shared.lock();
        if state.token.as_ref().is_some_and(|t| t.value == token) {
            state.token = None;
            match state.forced_at.map(|at| at + backoff) {
                Some(until) if now < until => {
                    state.retry_at = Some(state.retry_at.map_or(until, |at| at.max(until)));
                    state.error = Some(format!(
                        "the bucket refused two tokens within {} s",
                        backoff.as_secs()
                    ));
                }
                _ => {
                    state.forced_at = Some(now);
                    state.retry_at = None;
                }
            }
        }
        state.token.is_some() || state.requesting || state.retry_at.is_none_or(|at| now >= at)
    }

    /// Where tokens come from, as messages name it.
    #[cfg(test)]
    pub fn describe(&self) -> String {
        self.shared.describe()
    }
}

impl Shared {
    fn lock(&self) -> std::sync::MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// A valid token, or the error of the last request when none is valid
    /// and none may start yet, or `None` to wait for the request under way.
    /// Starts a request when one is due.
    fn poll(self: &Arc<Self>) -> Option<std::result::Result<String, String>> {
        let now = Instant::now();
        let mut state = self.lock();
        let may_start = !state.requesting && state.retry_at.is_none_or(|at| now >= at);
        if let Some(token) = state.token.as_ref().filter(|t| now < t.expires_at) {
            let value = token.value.clone();
            if now >= token.refresh_at && may_start {
                state.requesting = true;
                tokio::spawn(self.clone().request(false));
            }
            return Some(Ok(value));
        }
        if may_start {
            state.requesting = true;
            let first = !state.had_token && state.error.is_none();
            tokio::spawn(self.clone().request(first));
            return None;
        }
        if state.requesting {
            return None;
        }
        Some(Err(state.error.clone().unwrap_or_default()))
    }

    /// One request for a token, or with `first` the requests for a first
    /// one; records what it got and wakes the waiting calls. A request that
    /// ends without an answer, by a panic or by its task being dropped,
    /// counts as a failed one.
    async fn request(self: Arc<Self>, first: bool) {
        let mut ending = Ending {
            shared: self.clone(),
            recorded: false,
        };
        let got = if first {
            self.first_token().await
        } else {
            self.fetch(self.timing.attempt).await
        };
        self.record(&mut self.lock(), got);
        ending.recorded = true;
        #[cfg(test)]
        if let Some(hook) = self.after_record.get() {
            hook(&self);
        }
        drop(ending);
    }

    /// Records the end of the request under way.
    fn record(&self, state: &mut State, got: std::result::Result<Token, String>) {
        state.requesting = false;
        let error = match got {
            Ok(token) => {
                state.token = Some(token);
                state.had_token = true;
                state.retry_at = None;
                state.backoff = Duration::ZERO;
                state.error = None;
                return;
            }
            Err(error) => error,
        };
        let now = Instant::now();
        let backoff = if state.backoff.is_zero() {
            self.timing.backoff
        } else {
            (state.backoff * 2).min(self.timing.max_backoff)
        };
        state.backoff = backoff;
        state.retry_at = Some(now + backoff);
        let valid = state.token.as_ref().filter(|t| now < t.expires_at);
        let what = self.describe();
        if let Some(token) = valid {
            tracing::warn!(
                "cluster: a token from {what}: {error}; the current one expires in {} s; next \
                 attempt in {} s",
                (token.expires_at - now).as_secs(),
                backoff.as_secs()
            );
        } else {
            tracing::warn!(
                "cluster: a token from {what}: {error}; next attempt in {} s",
                backoff.as_secs()
            );
        }
        state.error = Some(error);
    }

    /// Requests a token until one is had or `timing.budget` is spent: no
    /// request starts after that, but each has all of `timing.attempt`, so
    /// that the error is that of a whole request.
    async fn first_token(&self) -> std::result::Result<Token, String> {
        let start = Instant::now();
        let deadline = start + self.timing.budget;
        let mut pause = self.timing.pause;
        let mut attempts = 0;
        loop {
            attempts += 1;
            let error = match self.fetch(self.timing.attempt).await {
                Ok(token) => return Ok(token),
                Err(error) => error,
            };
            if Instant::now() + pause >= deadline {
                let spent = start.elapsed().as_secs_f64();
                return Err(format!("{error} ({attempts} attempts in {spent:.1} s)"));
            }
            tokio::time::sleep(pause).await;
            pause = (pause * 2).min(self.timing.max_pause);
        }
    }

    /// Where tokens come from, for messages.
    fn describe(&self) -> String {
        match &self.source {
            Source::ServiceAccount { email, .. } => format!("service account {email}"),
            Source::Metadata { url } => format!("metadata server {url}"),
            #[cfg(test)]
            Source::Test(_) => "test endpoint".into(),
        }
    }

    /// One token request, of at most `timeout` from connecting to the end
    /// of the answer.
    async fn fetch(&self, timeout: Duration) -> std::result::Result<Token, String> {
        tokio::time::timeout(timeout, self.fetch_once(timeout))
            .await
            .unwrap_or_else(|_| {
                let timeout = timeout.as_secs_f64();
                Err(format!("no answer within {timeout:.1} s"))
            })
    }

    async fn fetch_once(&self, timeout: Duration) -> std::result::Result<Token, String> {
        let form = || {
            let content_type = "application/x-www-form-urlencoded";
            vec!["content-type".to_owned(), content_type.to_owned()]
        };
        let (method, url, headers, body) = match &self.source {
            Source::ServiceAccount {
                email,
                key_id,
                key,
                token_uri,
            } => {
                let jwt = assertion(key, email, key_id.as_deref(), token_uri, unix_s())?;
                let body = form_body(&[
                    ("grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer"),
                    ("assertion", &jwt),
                ]);
                ("POST", token_uri.clone(), form(), body)
            }
            Source::Metadata { url } => (
                "GET",
                url.clone(),
                vec!["metadata-flavor".into(), "Google".into()],
                String::new(),
            ),
            #[cfg(test)]
            Source::Test(answer) => return answer().await,
        };
        let body = Payload::Bytes(body.into_bytes().into());
        let response = self
            .http
            .send(method.into(), url, headers, body, None, (timeout, timeout))
            .await?;
        if response.status != 200 {
            let detail = error_text(&response.body);
            return Err(format!("{} {detail}", response.status));
        }
        parse_token(&response.body, Instant::now())
    }

    fn error(&self, detail: &str) -> Error {
        Error::Http(format!("token from {}: {detail}", self.describe()))
    }
}

/// A credentials file's contents as the source of its tokens.
fn parse_file(text: &str) -> std::result::Result<Source, String> {
    let file: File = serde_json::from_str(text).map_err(|e| e.to_string())?;
    let required = |value: Option<String>, name: &str| {
        value
            .filter(|v| !v.is_empty())
            .ok_or_else(|| format!("{} credentials have no {name}", file.kind))
    };
    let token_uri = file.token_uri.clone().unwrap_or_else(|| TOKEN_URI.into());
    if file.kind != "service_account" {
        return Err(format!(
            "credentials of type {:?} are not supported: use a service account key or the \
             metadata server",
            file.kind
        ));
    }
    check_token_uri(&token_uri)?;
    let pem = required(file.private_key.clone(), "private_key")?;
    let der = PrivatePkcs8KeyDer::from_pem_slice(pem.as_bytes())
        .map_err(|e| format!("private_key: {e}"))?;
    let key =
        RsaKeyPair::from_pkcs8(der.secret_pkcs8_der()).map_err(|e| format!("private_key: {e}"))?;
    Ok(Source::ServiceAccount {
        email: required(file.client_email.clone(), "client_email")?,
        key_id: file.private_key_id.clone(),
        key: Box::new(key),
        token_uri,
    })
}

/// Refuses a token endpoint that a credential would reach in the clear: one
/// that is not `https`, unless its host is a loopback address. Reads the URL
/// as the HTTP client does (`http::Uri`), and refuses one with a user name or
/// password, which no token endpoint needs.
fn check_token_uri(text: &str) -> std::result::Result<(), String> {
    let refuse = |why: &str| Err(format!("token_uri {text:?} {why}"));
    let Some(uri) = text
        .parse::<http::Uri>()
        .ok()
        .filter(|u| u.host().is_some())
    else {
        return refuse("is not an absolute URL");
    };
    let Some(authority) = uri.authority() else {
        return refuse("is not an absolute URL");
    };
    if authority.as_str().contains('@') {
        return refuse("must not hold a user name or password");
    }
    match uri.scheme_str() {
        Some("https") => Ok(()),
        Some("http") if is_loopback(authority.host()) => Ok(()),
        _ => refuse("must be https, or http on a loopback address"),
    }
}

/// `localhost` or a loopback IP address, IPv6 in brackets.
fn is_loopback(host: &str) -> bool {
    let bare = host.strip_prefix('[').and_then(|h| h.strip_suffix(']'));
    host.eq_ignore_ascii_case("localhost")
        || bare
            .unwrap_or(host)
            .parse::<std::net::IpAddr>()
            .is_ok_and(|ip| ip.is_loopback())
}

fn metadata_source(host: &str) -> Source {
    let url = format!("http://{host}/computeMetadata/v1/instance/service-accounts/default/token");
    Source::Metadata { url }
}

/// A token endpoint's answer, received at `now`.
pub fn parse_token(body: &[u8], now: Instant) -> std::result::Result<Token, String> {
    let answer: Answer =
        serde_json::from_slice(body).map_err(|e| format!("unexpected answer: {e}"))?;
    if answer.access_token.is_empty() {
        return Err("the answer has an empty access_token".into());
    }
    if answer.expires_in == 0 {
        return Err("the answer's token has expired".into());
    }
    let lifetime = Duration::from_secs(answer.expires_in);
    Ok(Token::new(answer.access_token, now, lifetime))
}

/// The JWT a service account signs to ask `audience` for a token at
/// `now_s`, in seconds since the epoch.
pub fn assertion(
    key: &RsaKeyPair,
    email: &str,
    key_id: Option<&str>,
    audience: &str,
    now_s: u64,
) -> std::result::Result<String, String> {
    let mut header = serde_json::json!({ "alg": "RS256", "typ": "JWT" });
    if let Some(key_id) = key_id {
        header["kid"] = key_id.into();
    }
    let claims = serde_json::json!({
        "iss": email,
        "scope": SCOPE,
        "aud": audience,
        "iat": now_s,
        "exp": now_s + JWT_LIFETIME_S,
    });
    let signed = format!(
        "{}.{}",
        URL_SAFE_NO_PAD.encode(header.to_string()),
        URL_SAFE_NO_PAD.encode(claims.to_string())
    );
    let mut signature = vec![0; key.public().modulus_len()];
    key.sign(
        &RSA_PKCS1_SHA256,
        &SystemRandom::new(),
        signed.as_bytes(),
        &mut signature,
    )
    .map_err(|_| "could not sign the JWT".to_owned())?;
    Ok(format!("{signed}.{}", URL_SAFE_NO_PAD.encode(signature)))
}

/// `application/x-www-form-urlencoded`.
fn form_body(fields: &[(&str, &str)]) -> String {
    let fields = fields
        .iter()
        .map(|(name, value)| format!("{}={}", encode(name, true), encode(value, true)));
    fields.collect::<Vec<_>>().join("&")
}

/// What an error answer says, at most `MAX_ERROR_TEXT` characters of it:
/// OAuth's `error` and `error_description`, or a Google API error's `status`
/// and `message`. Nothing else of the body: an endpoint may echo a request's
/// credentials in it.
fn error_text(body: &[u8]) -> String {
    #[derive(Deserialize)]
    #[serde(untagged)]
    enum Detail {
        OAuth(String),
        Api {
            status: Option<String>,
            message: Option<String>,
        },
    }
    #[derive(Deserialize)]
    struct ErrorAnswer {
        error: Detail,
        error_description: Option<String>,
    }
    let Ok(ErrorAnswer {
        error,
        error_description,
    }) = serde_json::from_slice(body)
    else {
        return String::new();
    };
    let parts = match error {
        Detail::OAuth(error) => [Some(error), error_description],
        Detail::Api { status, message } => [status, message],
    };
    let text = parts.into_iter().flatten().collect::<Vec<_>>().join(": ");
    text.chars().take(MAX_ERROR_TEXT).collect()
}

fn unix_s() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

#[cfg(test)]
#[path = "google_auth-test.rs"]
mod tests;
