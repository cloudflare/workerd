// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! OAuth 2.0 access tokens for Cloud Storage, from the credentials file
//! `GOOGLE_APPLICATION_CREDENTIALS` names, else from the one `gcloud auth
//! application-default login` writes (`gcloud_credentials_file`), else from
//! the metadata server of the Compute Engine VM or GKE pod the process runs
//! on. GKE's Workload Identity serves the token of the pod's Kubernetes
//! service account there.
//!
//! A credentials file holds a service account's key, whose signed JWT is
//! exchanged for a token (the JWT bearer grant, RFC 7523); or the user
//! credentials `gcloud auth application-default login` writes, whose refresh
//! token is; or, as that command writes with
//! `--impersonate-service-account`, either of those as the source of a
//! token for another service account (`impersonated_service_account`).
//! Other kinds of credentials file (workload identity federation) are
//! refused. A URL a file names must be `https`, or on a loopback address.
//! Of its impersonation URL only the service account is taken: the request
//! goes to IAM, not to the URL's host unless that is a loopback address.
//! The `quota_project_id` of user credentials, and the top-level one of an
//! `impersonated_service_account` file, is the project billed for the
//! bucket's requests (`quota_project`), as Google's client libraries bill
//! it; theirs do not read one from a service account's key.
//!
//! `GOOGLE_IMPERSONATE_SERVICE_ACCOUNT` makes any of these the source of a
//! token for the service account it names (`impersonate`). An impersonated
//! token comes from IAM's `generateAccessToken`, which the source token
//! calls; it needs the `cloud-platform` scope, which a service account's JWT
//! asks for and `gcloud auth application-default login` grants; a metadata
//! server's token has the VM's access scopes, or under GKE Workload Identity
//! `cloud-platform`. An impersonated token lasts until its `expireTime` by
//! IAM's clock, the answer's `Date`, and at most the hour it asks for.
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
//! Off Google Cloud, the metadata server is not there: when nothing suggests
//! that it is (`google_cloud`), the first request for a first token from it
//! is preceded by a connection to it, and when that is refused or does not
//! connect within a second, the request gives up at once, saying how to
//! provide credentials, instead of retrying. At the default address, on
//! Google Cloud or off it, an answer without Google's `Metadata-Flavor:
//! Google` header is from another server there, such as another cloud's
//! metadata server, unless its status says to try again (429, 5xx): a first
//! token is given up on at once in the same way, and any other request
//! fails. A metadata server `GCE_METADATA_HOST` names, such as a proxy or an
//! emulator, need not send the header.
//!
//! A token the bucket refuses (401) is dropped, and the next call requests
//! another, once, even during a pause after a failed request; a failure is
//! not retried but starts the usual pause. When a second token is refused
//! within 10 s of that, bucket calls wait for the pause instead.

use std::path::PathBuf;
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
use crate::client::http_date_ms;
use crate::http::Http;
use crate::http::Payload;

/// The metadata server's address on Compute Engine and GKE, unless
/// `GCE_METADATA_HOST` names another.
pub const METADATA_HOST: &str = "169.254.169.254";
/// Where a credentials file without a `token_uri` exchanges its grant.
const TOKEN_URI: &str = "https://oauth2.googleapis.com/token";
/// Reading, writing, listing and deleting objects.
const SCOPE: &str = "https://www.googleapis.com/auth/devstorage.read_write";
/// Every API, as a token that impersonates another service account needs.
const CLOUD_PLATFORM: &str = "https://www.googleapis.com/auth/cloud-platform";
/// IAM's Service Account Credentials API.
const IAM_CREDENTIALS: &str = "https://iamcredentials.googleapis.com";
/// The path of a service account's `generateAccessToken`, before and after
/// its email address or unique ID.
const GENERATE_PATH: (&str, &str) = ("/v1/projects/-/serviceAccounts/", ":generateAccessToken");
/// The lifetime an impersonated token asks for, the most IAM grants without
/// an organization policy that allows longer.
const IMPERSONATED_LIFETIME_S: u64 = 3600;
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

pub enum Source {
    /// A service account's key: it signs a JWT for `scope` that `token_uri`
    /// exchanges.
    ServiceAccount {
        email: String,
        key_id: Option<String>,
        key: Box<RsaKeyPair>,
        token_uri: String,
        scope: &'static str,
    },
    /// A user's OAuth client and refresh token, as gcloud stores them.
    AuthorizedUser {
        client_id: String,
        client_secret: String,
        refresh_token: String,
        token_uri: String,
        quota_project: Option<String>,
    },
    /// The metadata server's token endpoint; with `probe`, its `host:port`,
    /// which may have nothing there. With `flavor`, an answer that is not
    /// to be retried must carry `Metadata-Flavor: Google`.
    Metadata {
        url: String,
        probe: Option<String>,
        flavor: bool,
    },
    /// A token for the service account `target`, which `url`
    /// (`generateAccessToken`) answers for a token from `source`, one of the
    /// others. `delegates` are the resource names of the service accounts
    /// between them, in order. The source's quota project, if any, is
    /// billed for the call (`x-goog-user-project`), as Google's client
    /// libraries do; `quota_project`, for the bucket's requests.
    Impersonated {
        source: Box<Self>,
        target: String,
        delegates: Vec<String>,
        url: String,
        quota_project: Option<String>,
    },
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

/// A failed token request.
struct Failure {
    text: String,
    /// Another request may answer differently; `false` for an answer that
    /// says the source is not there.
    retry: bool,
}

impl From<String> for Failure {
    fn from(text: String) -> Self {
        Self { text, retry: true }
    }
}

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
    client_id: Option<String>,
    client_secret: Option<String>,
    refresh_token: Option<String>,
    token_uri: Option<String>,
    service_account_impersonation_url: Option<String>,
    source_credentials: Option<serde_json::Value>,
    delegates: Option<Vec<String>>,
    quota_project_id: Option<String>,
}

/// A token endpoint's answer.
#[derive(Deserialize)]
struct Answer {
    access_token: String,
    expires_in: u64,
}

/// `generateAccessToken`'s answer.
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Impersonated {
    access_token: String,
    expire_time: String,
}

impl Credentials {
    /// Tokens from `source`, over `http`.
    pub fn new(source: Source, http: Http) -> Self {
        Self::over(source, http, Timing::default())
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

    /// The project billed for requests made with these credentials
    /// (`x-goog-user-project`), if any.
    pub fn quota_project(&self) -> Option<&str> {
        match &self.shared.source {
            Source::AuthorizedUser { quota_project, .. }
            | Source::Impersonated { quota_project, .. } => quota_project.as_deref(),
            _ => None,
        }
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
            self.fetch(self.timing.attempt).await.map_err(|f| f.text)
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
    /// that the error is that of a whole request. An answer saying that the
    /// source is not there ends the requests at once.
    async fn first_token(&self) -> std::result::Result<Token, String> {
        let (source, step) = match &self.source {
            Source::Impersonated { source, .. } => (&**source, SOURCE_STEP),
            source => (source, ""),
        };
        if let Source::Metadata {
            probe: Some(address),
            ..
        } = source
        {
            probe(address).await.map_err(|e| format!("{step}{e}"))?;
        }
        let start = Instant::now();
        let deadline = start + self.timing.budget;
        let mut pause = self.timing.pause;
        let mut attempts = 0;
        loop {
            attempts += 1;
            let error = match self.fetch(self.timing.attempt).await {
                Ok(token) => return Ok(token),
                Err(Failure { text, retry: false }) => {
                    let detail = text.strip_prefix(step).unwrap_or(&text);
                    return Err(format!("{step}no credentials found: {detail}. {HINT}"));
                }
                Err(Failure { text, .. }) => text,
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
        describe(&self.source)
    }

    /// One token request, of at most `timeout` from connecting to the end
    /// of the answer.
    async fn fetch(&self, timeout: Duration) -> std::result::Result<Token, Failure> {
        tokio::time::timeout(timeout, self.fetch_once(timeout))
            .await
            .unwrap_or_else(|_| {
                let timeout = timeout.as_secs_f64();
                Err(format!("no answer within {timeout:.1} s").into())
            })
    }

    async fn fetch_once(&self, timeout: Duration) -> std::result::Result<Token, Failure> {
        match &self.source {
            Source::Impersonated {
                source,
                delegates,
                url,
                ..
            } => {
                let token = token_from(&self.http, source, timeout)
                    .await
                    .map_err(|f| Failure {
                        text: format!("{SOURCE_STEP}{}", f.text),
                        ..f
                    })?;
                let quota_project = match &**source {
                    Source::AuthorizedUser { quota_project, .. } => quota_project.as_deref(),
                    _ => None,
                };
                let request = Impersonation {
                    bearer: &token.value,
                    delegates,
                    url,
                    quota_project,
                };
                Ok(impersonated(&self.http, &request, timeout)
                    .await
                    .map_err(|e| format!("impersonation: {e}"))?)
            }
            #[cfg(test)]
            Source::Test(answer) => Ok(answer().await?),
            source => token_from(&self.http, source, timeout).await,
        }
    }

    fn error(&self, detail: &str) -> Error {
        Error::Http(format!("token from {}: {detail}", self.describe()))
    }
}

/// A credentials file's `quota_project_id`, unless empty: a project ID, as
/// it goes into a header.
fn quota_project(id: Option<String>) -> std::result::Result<Option<String>, String> {
    let Some(id) = id.filter(|id| !id.is_empty()) else {
        return Ok(None);
    };
    let allowed = |c: char| c.is_ascii_lowercase() || c.is_ascii_digit() || ".:-".contains(c);
    if !id.chars().all(allowed) {
        return Err(format!("quota_project_id {id:?} is not a project ID"));
    }
    Ok(Some(id))
}

/// The step of an impersonated token that gets its source's token, as
/// messages name it.
const SOURCE_STEP: &str = "source token: ";

/// Where tokens from `source` come from, for messages.
fn describe(source: &Source) -> String {
    match source {
        Source::ServiceAccount { email, .. } => format!("service account {email}"),
        Source::AuthorizedUser { .. } => "authorized_user credentials".into(),
        Source::Metadata { url, .. } => format!("metadata server {url}"),
        Source::Impersonated { source, target, .. } => {
            format!(
                "service account {target}, impersonated with {}",
                describe(source)
            )
        }
        #[cfg(test)]
        Source::Test(_) => "test endpoint".into(),
    }
}

/// One request for a token from `source`, which does not impersonate.
async fn token_from(
    http: &Http,
    source: &Source,
    timeout: Duration,
) -> std::result::Result<Token, Failure> {
    let form = || {
        let content_type = "application/x-www-form-urlencoded";
        vec!["content-type".to_owned(), content_type.to_owned()]
    };
    let (method, url, headers, body) = match source {
        Source::ServiceAccount {
            email,
            key_id,
            key,
            token_uri,
            scope,
            ..
        } => {
            let jwt = assertion(key, email, key_id.as_deref(), token_uri, scope, unix_s())?;
            let body = form_body(&[
                ("grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer"),
                ("assertion", &jwt),
            ]);
            ("POST", token_uri.clone(), form(), body)
        }
        Source::AuthorizedUser {
            client_id,
            client_secret,
            refresh_token,
            token_uri,
            ..
        } => {
            let body = form_body(&[
                ("grant_type", "refresh_token"),
                ("client_id", client_id),
                ("client_secret", client_secret),
                ("refresh_token", refresh_token),
            ]);
            ("POST", token_uri.clone(), form(), body)
        }
        Source::Metadata { url, .. } => (
            "GET",
            url.clone(),
            vec!["metadata-flavor".into(), "Google".into()],
            String::new(),
        ),
        Source::Impersonated { .. } => {
            return Err("an impersonated token has no source".to_owned().into());
        }
        #[cfg(test)]
        Source::Test(answer) => return Ok(answer().await?),
    };
    let body = Payload::Bytes(body.into_bytes().into());
    let response = http
        .send(method.into(), url, headers, body, None, (timeout, timeout))
        .await?;
    let retryable = response.status == 429 || response.status >= 500;
    let flavor = matches!(source, Source::Metadata { flavor: true, .. });
    if flavor && !retryable && response.metadata_flavor != "Google" {
        return Err(Failure {
            text: format!(
                "the {} answer is not from Google's metadata server, having no \
                 `Metadata-Flavor: Google` header",
                response.status
            ),
            retry: false,
        });
    }
    if response.status != 200 {
        let detail = error_text(&response.body);
        return Err(format!("{} {detail}", response.status).into());
    }
    Ok(parse_token(&response.body, Instant::now())?)
}

/// A `generateAccessToken` request.
struct Impersonation<'a> {
    /// The source token.
    bearer: &'a str,
    delegates: &'a [String],
    url: &'a str,
    /// The project billed for the call.
    quota_project: Option<&'a str>,
}

/// One `generateAccessToken` request.
async fn impersonated(
    http: &Http,
    request: &Impersonation<'_>,
    timeout: Duration,
) -> std::result::Result<Token, String> {
    let lifetime = format!("{IMPERSONATED_LIFETIME_S}s");
    let mut body = serde_json::json!({ "scope": [SCOPE], "lifetime": lifetime });
    if !request.delegates.is_empty() {
        body["delegates"] = request.delegates.into();
    }
    let mut headers = vec![
        "authorization".to_owned(),
        format!("Bearer {}", request.bearer),
        "content-type".to_owned(),
        "application/json".to_owned(),
    ];
    if let Some(project) = request.quota_project {
        headers.extend(["x-goog-user-project".to_owned(), project.to_owned()]);
    }
    let body = Payload::Bytes(body.to_string().into_bytes().into());
    let response = http
        .send(
            "POST".into(),
            request.url.into(),
            headers,
            body,
            None,
            (timeout, timeout),
        )
        .await?;
    if response.status != 200 {
        let detail = error_text(&response.body);
        return Err(format!("{} {detail}", response.status));
    }
    let date = http_date_ms(&response.date);
    parse_impersonated(&response.body, date, SystemTime::now(), Instant::now())
}

/// `generateAccessToken`'s answer, received at `now`. Its `expireTime` is
/// on IAM's clock: the token's lifetime is that less the answer's `Date`,
/// `date` in Unix milliseconds, or less this machine's clock, `wall`,
/// without one. It is at most the lifetime asked for.
pub fn parse_impersonated(
    body: &[u8],
    date: Option<i64>,
    wall: SystemTime,
    now: Instant,
) -> std::result::Result<Token, String> {
    let answer: Impersonated =
        serde_json::from_slice(body).map_err(|e| format!("unexpected answer: {e}"))?;
    if answer.access_token.is_empty() {
        return Err("the answer has an empty accessToken".into());
    }
    let expires = parse_rfc3339_ms(&answer.expire_time).ok_or_else(|| {
        format!(
            "the answer's expireTime {:?} is not a time",
            answer.expire_time
        )
    })?;
    let sent = date.unwrap_or_else(|| {
        let wall = wall.duration_since(UNIX_EPOCH).unwrap_or_default();
        i64::try_from(wall.as_millis()).unwrap_or(i64::MAX)
    });
    let left = expires.saturating_sub(sent);
    if left <= 0 {
        return Err(format!(
            "the answer's token has expired (expireTime {})",
            answer.expire_time
        ));
    }
    let lifetime = Duration::from_millis(left.unsigned_abs())
        .min(Duration::from_secs(IMPERSONATED_LIFETIME_S));
    Ok(Token::new(answer.access_token, now, lifetime))
}

/// An RFC 3339 date and time, `2026-11-10T01:02:03.5Z` or with an offset
/// (`+01:00`), in milliseconds since the epoch. A leap second, `60`, is taken
/// only at `23:59`, as the next second.
pub fn parse_rfc3339_ms(text: &str) -> Option<i64> {
    let number = |range: std::ops::Range<usize>| -> Option<i64> {
        let digits = text.get(range)?;
        digits
            .bytes()
            .all(|b| b.is_ascii_digit())
            .then(|| digits.parse().ok())?
    };
    let bytes = text.as_bytes();
    let separators = [(4, b'-'), (7, b'-'), (13, b':'), (16, b':')];
    if bytes.len() < 20 || separators.iter().any(|&(at, c)| bytes[at] != c) {
        return None;
    }
    if !matches!(bytes[10], b'T' | b't') {
        return None;
    }
    let (year, month, day) = (number(0..4)?, number(5..7)?, number(8..10)?);
    let (hour, minute, second) = (number(11..13)?, number(14..16)?, number(17..19)?);
    let valid = (1..=12).contains(&month)
        && (1..=days_in_month(year, month)).contains(&day)
        && hour < 24
        && minute < 60
        && (second < 60 || (second, minute, hour) == (60, 59, 23));
    if !valid {
        return None;
    }
    let mut rest = &text[19..];
    let mut millis = 0;
    if let Some(fraction) = rest.strip_prefix('.') {
        let digits = fraction.bytes().take_while(u8::is_ascii_digit).count();
        if digits == 0 {
            return None;
        }
        let first: String = fraction[..digits]
            .chars()
            .chain("00".chars())
            .take(3)
            .collect();
        millis = first.parse().ok()?;
        rest = &fraction[digits..];
    }
    let offset_s = match rest {
        "Z" | "z" => 0,
        _ => {
            let sign = match rest.as_bytes().first()? {
                b'+' => 1,
                b'-' => -1,
                _ => return None,
            };
            let offset = rest.get(1..)?.as_bytes();
            if offset.len() != 5 || offset[2] != b':' {
                return None;
            }
            let digits = |at: usize| -> Option<i64> {
                let pair = &offset[at..at + 2];
                pair.iter()
                    .all(u8::is_ascii_digit)
                    .then(|| i64::from(pair[0] - b'0') * 10 + i64::from(pair[1] - b'0'))
            };
            let (hours, minutes) = (digits(0)?, digits(3)?);
            if hours > 23 || minutes > 59 {
                return None;
            }
            sign * (hours * 3600 + minutes * 60)
        }
    };
    let seconds = days_from_civil(year, month, day) * 86_400 + hour * 3600 + minute * 60 + second;
    Some((seconds - offset_s) * 1000 + millis)
}

/// The number of days in `month` of `year`, in the proleptic Gregorian
/// calendar.
fn days_in_month(year: i64, month: i64) -> i64 {
    let leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    match month {
        2 if leap => 29,
        2 => 28,
        4 | 6 | 9 | 11 => 30,
        _ => 31,
    }
}

/// Days since 1970-01-01 of a date in the proleptic Gregorian calendar.
fn days_from_civil(year: i64, month: i64, day: i64) -> i64 {
    let year = if month <= 2 { year - 1 } else { year };
    let era = year.div_euclid(400);
    let year_of_era = year - era * 400;
    let month_from_march = (month + 9) % 12;
    let day_of_year = (153 * month_from_march + 2) / 5 + day - 1;
    let day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    era * 146_097 + day_of_era - 719_468
}

/// A credentials file's contents as the source of its tokens.
pub fn parse_file(text: &str) -> std::result::Result<Source, String> {
    let file: File = serde_json::from_str(text).map_err(|e| e.to_string())?;
    if file.kind != "impersonated_service_account" {
        return parse_source(&file, SCOPE);
    }
    let url = file
        .service_account_impersonation_url
        .filter(|url| !url.is_empty())
        .ok_or(
            "impersonated_service_account credentials have no service_account_impersonation_url",
        )?;
    let (target, url) = impersonation_url(&url)?;
    let delegates = file.delegates.unwrap_or_default();
    let delegates = delegates
        .iter()
        .map(|d| delegate(d))
        .collect::<std::result::Result<_, _>>()?;
    let inner = file
        .source_credentials
        .ok_or("impersonated_service_account credentials have no source_credentials")?;
    let inner: File =
        serde_json::from_value(inner).map_err(|e| format!("source_credentials: {e}"))?;
    if inner.kind == "impersonated_service_account" {
        return Err(
            "source_credentials of type \"impersonated_service_account\" are not supported".into(),
        );
    }
    let source =
        parse_source(&inner, CLOUD_PLATFORM).map_err(|e| format!("source_credentials: {e}"))?;
    Ok(Source::Impersonated {
        source: Box::new(source),
        target,
        delegates,
        url,
        quota_project: quota_project(file.quota_project_id)?,
    })
}

/// A credentials file's contents, which do not impersonate, as the source
/// of tokens for `scope`.
fn parse_source(file: &File, scope: &'static str) -> std::result::Result<Source, String> {
    let required = |value: Option<String>, name: &str| {
        value
            .filter(|v| !v.is_empty())
            .ok_or_else(|| format!("{} credentials have no {name}", file.kind))
    };
    let token_uri = file.token_uri.clone().unwrap_or_else(|| TOKEN_URI.into());
    if !matches!(file.kind.as_str(), "service_account" | "authorized_user") {
        return Err(format!(
            "credentials of type {:?} are not supported: use a service account key, \
             authorized_user or impersonated_service_account credentials, or the metadata server",
            file.kind
        ));
    }
    check_url("token_uri", &token_uri)?;
    Ok(if file.kind == "service_account" {
        let pem = required(file.private_key.clone(), "private_key")?;
        let der = PrivatePkcs8KeyDer::from_pem_slice(pem.as_bytes())
            .map_err(|e| format!("private_key: {e}"))?;
        let key = RsaKeyPair::from_pkcs8(der.secret_pkcs8_der())
            .map_err(|e| format!("private_key: {e}"))?;
        Source::ServiceAccount {
            email: required(file.client_email.clone(), "client_email")?,
            key_id: file.private_key_id.clone(),
            key: Box::new(key),
            token_uri,
            scope,
        }
    } else {
        Source::AuthorizedUser {
            client_id: required(file.client_id.clone(), "client_id")?,
            client_secret: required(file.client_secret.clone(), "client_secret")?,
            refresh_token: required(file.refresh_token.clone(), "refresh_token")?,
            token_uri,
            quota_project: quota_project(file.quota_project_id.clone())?,
        }
    })
}

/// `source` as the source of tokens for the service account `chain` names,
/// as `GOOGLE_IMPERSONATE_SERVICE_ACCOUNT` does: an email address, or as
/// gcloud's `--impersonate-service-account` takes them, a comma-separated
/// list of them, each of which may impersonate the next, the last the
/// target.
pub fn impersonate(source: Source, chain: &str) -> std::result::Result<Source, String> {
    let accounts: Vec<_> = chain.split(',').map(str::trim).collect();
    let (target, delegates) = accounts.split_last().ok_or("no service account")?;
    let target = account(target)?;
    let delegates = delegates
        .iter()
        .map(|d| delegate(d))
        .collect::<std::result::Result<_, _>>()?;
    let source = match source {
        Source::Impersonated {
            target: already, ..
        } => {
            return Err(format!("the credentials impersonate {already} already"));
        }
        Source::ServiceAccount {
            email,
            key_id,
            key,
            token_uri,
            ..
        } => Source::ServiceAccount {
            email,
            key_id,
            key,
            token_uri,
            scope: CLOUD_PLATFORM,
        },
        source => source,
    };
    let url = generate_url(&target);
    Ok(Source::Impersonated {
        source: Box::new(source),
        target,
        delegates,
        url,
        quota_project: None,
    })
}

/// IAM's `generateAccessToken` for the service account `account`.
fn generate_url(account: &str) -> String {
    let (before, after) = GENERATE_PATH;
    format!("{IAM_CREDENTIALS}{before}{account}{after}")
}

/// A file's `service_account_impersonation_url` as the service account it
/// names and the URL to request its tokens from. Its path must be that of a
/// service account's `generateAccessToken`, with no query or fragment. The
/// request goes to IAM itself, not to the file's host, unless that is a
/// loopback address (an emulator or a test): a file does not choose where
/// the source token is sent.
fn impersonation_url(text: &str) -> std::result::Result<(String, String), String> {
    const FIELD: &str = "service_account_impersonation_url";
    check_url(FIELD, text)?;
    let refuse = || format!("{FIELD} {text:?} is not a service account's generateAccessToken URL");
    let uri: http::Uri = text.parse().map_err(|_| refuse())?;
    // `http::Uri` drops a fragment.
    let path = uri
        .path_and_query()
        .filter(|p| p.query().is_none() && !text.contains('#'));
    let (before, after) = GENERATE_PATH;
    let target = path
        .and_then(|p| p.path().strip_prefix(before))
        .and_then(|rest| rest.strip_suffix(after))
        .ok_or_else(refuse)?;
    let target = account(target).map_err(|e| format!("{FIELD}: {e}"))?;
    let url = if uri.host().is_some_and(is_loopback) {
        text.to_owned()
    } else {
        generate_url(&target)
    };
    Ok((target, url))
}

/// A service account's email address or numeric unique ID, which goes into
/// a URL's path as it is.
fn account(account: &str) -> std::result::Result<String, String> {
    let part = |text: &str| {
        let allowed = |c: char| c.is_ascii_alphanumeric() || ".-_".contains(c);
        !text.is_empty() && text.chars().all(allowed)
    };
    let email = account
        .split_once('@')
        .is_some_and(|(local, domain)| part(local) && part(domain));
    let id = !account.is_empty() && account.bytes().all(|b| b.is_ascii_digit());
    (email || id)
        .then(|| account.to_owned())
        .ok_or_else(|| format!("{account:?} is not a service account's email address or unique ID"))
}

/// A delegate's resource name, from it or from the service account's email
/// address or unique ID.
fn delegate(delegate: &str) -> std::result::Result<String, String> {
    let name = delegate
        .strip_prefix("projects/-/serviceAccounts/")
        .unwrap_or(delegate);
    Ok(format!("projects/-/serviceAccounts/{}", account(name)?))
}

/// Refuses a URL from a credentials file, `field`, that a credential would
/// reach in the clear: one that is not `https`, unless its host is a
/// loopback address. Reads the URL as the HTTP client does (`http::Uri`),
/// and refuses one with a user name or password, which no endpoint needs.
fn check_url(field: &str, text: &str) -> std::result::Result<(), String> {
    let refuse = |why: &str| Err(format!("{field} {text:?} {why}"));
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

/// The metadata server at `named`, which `GCE_METADATA_HOST` names, else at
/// `METADATA_HOST`, where an answer without `Metadata-Flavor: Google` is from
/// something else. Unless `on_google`, which says the process runs on Google
/// Cloud, a first token is not waited for when nothing is there.
pub fn metadata_server(named: Option<&str>, on_google: bool) -> Source {
    let mut source = metadata_source(named.unwrap_or(METADATA_HOST), on_google);
    if let Source::Metadata { flavor, .. } = &mut source {
        *flavor = named.is_none();
    }
    source
}

/// The metadata server at `host`, as at the default address.
fn metadata_source(host: &str, on_google: bool) -> Source {
    let url = format!("http://{host}/computeMetadata/v1/instance/service-accounts/default/token");
    let probe = (!on_google).then(|| {
        let port = if host.rsplit_once(':').is_some_and(|(_, p)| !p.contains(']')) {
            ""
        } else {
            ":80"
        };
        format!("{host}{port}")
    });
    Source::Metadata {
        url,
        probe,
        flavor: true,
    }
}

/// How long a connection to a metadata server that may not be there has.
const PROBE_TIMEOUT: Duration = Duration::from_secs(1);
/// How to provide credentials, for an error that finds none.
const HINT: &str = "Set GOOGLE_APPLICATION_CREDENTIALS to a credentials file, run `gcloud auth \
                    application-default login`, or run on Compute Engine or GKE";

/// Connects to the metadata server at `address`, `host:port`, which may not
/// be there: an error when nothing answers within `PROBE_TIMEOUT`.
async fn probe(address: &str) -> std::result::Result<(), String> {
    let connected = tokio::time::timeout(PROBE_TIMEOUT, tokio::net::TcpStream::connect(address));
    let problem = match connected.await {
        Ok(Ok(_)) => return Ok(()),
        Ok(Err(e)) => e.to_string(),
        Err(_) => format!("no connection within {} s", PROBE_TIMEOUT.as_secs()),
    };
    Err(format!(
        "no credentials found: nothing is at the metadata server {address} ({problem}). {HINT}"
    ))
}

/// Where `gcloud auth application-default login` writes its credentials:
/// `application_default_credentials.json` in gcloud's configuration
/// directory, `CLOUDSDK_CONFIG`, else `%APPDATA%\gcloud` on `windows`, else
/// `$HOME/.config/gcloud`.
pub fn gcloud_credentials_file(
    env: &dyn Fn(&str) -> Option<String>,
    windows: bool,
) -> Option<PathBuf> {
    let env = |name| env(name).filter(|value| !value.is_empty());
    let directory = match env("CLOUDSDK_CONFIG") {
        Some(directory) => PathBuf::from(directory),
        None if windows => PathBuf::from(env("APPDATA")?).join("gcloud"),
        None => PathBuf::from(env("HOME")?).join(".config").join("gcloud"),
    };
    Some(directory.join("application_default_credentials.json"))
}

/// Variables that suggest Google Cloud when set: `GCE_METADATA_HOST`,
/// Kubernetes', and those of Cloud Run services and jobs, Cloud Run functions
/// and App Engine.
const GOOGLE_CLOUD_VARIABLES: [&str; 8] = [
    "GCE_METADATA_HOST",
    "KUBERNETES_SERVICE_HOST",
    "K_SERVICE",
    "CLOUD_RUN_JOB",
    "FUNCTION_TARGET",
    "FUNCTION_NAME",
    "GAE_ENV",
    "GAE_SERVICE",
];

/// Whether the process runs on Google Cloud, where a metadata server may
/// refuse connections for a pod's first seconds: one of
/// `GOOGLE_CLOUD_VARIABLES` is set, or the machine's DMI product name,
/// `dmi_product`, is Google's.
pub fn google_cloud(env: &dyn Fn(&str) -> Option<String>, dmi_product: Option<&str>) -> bool {
    let set = |name| env(name).is_some_and(|value| !value.is_empty());
    GOOGLE_CLOUD_VARIABLES.into_iter().any(set)
        || dmi_product.is_some_and(|name| name.trim().starts_with("Google"))
}

/// The DMI product name of the machine, where Linux shows it.
pub fn dmi_product() -> Option<String> {
    std::fs::read_to_string("/sys/class/dmi/id/product_name").ok()
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

/// The JWT a service account signs to ask `audience` for a token for
/// `scope` at `now_s`, in seconds since the epoch.
pub fn assertion(
    key: &RsaKeyPair,
    email: &str,
    key_id: Option<&str>,
    audience: &str,
    scope: &str,
    now_s: u64,
) -> std::result::Result<String, String> {
    let mut header = serde_json::json!({ "alg": "RS256", "typ": "JWT" });
    if let Some(key_id) = key_id {
        header["kid"] = key_id.into();
    }
    let claims = serde_json::json!({
        "iss": email,
        "scope": scope,
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
