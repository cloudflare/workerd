// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Cloud Storage's dialect of the bucket client (`client.rs`): its XML API,
//! with OAuth 2.0 tokens (`google_auth.rs`).
//!
//! Cloud Storage does not apply `If-Match` or `If-None-Match` to a PUT, so an
//! object's version is its generation, the number Cloud Storage gives each
//! write of the object and answers in `x-goog-generation`, and a write's
//! precondition is `x-goog-if-generation-match`. `Guard::Absent` sends `0`,
//! which holds only while no live object has the name; `Guard::Match` sends
//! the generation, which holds only while the live object is that write. A
//! generation never comes back, even for the same bytes, where S3's `ETag`
//! of identical bytes repeats.
//!
//! Requests go to `https://storage.googleapis.com`, or to the emulator
//! `STORAGE_EMULATOR_HOST` names. Requests to an emulator carry no token
//! unless `GOOGLE_APPLICATION_CREDENTIALS` or `GCE_METADATA_HOST` names
//! where tokens come from.

use std::path::PathBuf;

use crate::Error;
use crate::Result;
use crate::bucket::Guard;
use crate::bucket::Version;
use crate::client::Client;
use crate::client::Dialect;
use crate::client::Request;
use crate::google_auth;
use crate::google_auth::Credentials;
use crate::http::Http;
use crate::http::HttpResponse;

const ENDPOINT: &str = "https://storage.googleapis.com";

/// A Cloud Storage bucket's client.
pub type Gcs = Client<Google>;

/// Authorizes requests with a bearer token.
pub struct Google {
    /// `None` for an emulator that takes no token.
    credentials: Option<Credentials>,
}

impl Gcs {
    /// `url` is `gs://<bucket>/<prefix>`; answers the client and the prefix.
    /// A `gs://` bucket takes no options: credentials come from the
    /// environment (`google_auth.rs`).
    pub fn new(url: &str, options: &[(String, String)]) -> Result<(Self, String)> {
        Self::with_env(url, options, &|name| std::env::var(name).ok())
    }

    /// As `new`, with `env` for the environment.
    pub(crate) fn with_env(
        url: &str,
        options: &[(String, String)],
        env: &dyn Fn(&str) -> Option<String>,
    ) -> Result<(Self, String)> {
        let (bucket, prefix) = parse_url(url)?;
        if let Some((name, _)) = options.first() {
            return Err(Error::Config(format!(
                "a gs:// bucket takes no bucketOptions, but {name} is set: its credentials come \
                 from GOOGLE_APPLICATION_CREDENTIALS or the metadata server"
            )));
        }
        let env = |name: &str| env(name).filter(|value| !value.is_empty());
        let emulator = env("STORAGE_EMULATOR_HOST");
        let metadata = env("GCE_METADATA_HOST");
        let http = Http::new().map_err(|e| Error::Config(e.to_string()))?;
        let credentials = match env("GOOGLE_APPLICATION_CREDENTIALS") {
            Some(path) => Some(Credentials::from_file(&PathBuf::from(path), http.clone())?),
            None if emulator.is_some() && metadata.is_none() => None,
            None => Some(Credentials::metadata(
                metadata.as_deref().unwrap_or(google_auth::METADATA_HOST),
                http.clone(),
            )),
        };
        let endpoint = match emulator {
            Some(host) if host.contains("://") => host,
            Some(host) => format!("http://{host}"),
            None => ENDPOINT.to_owned(),
        };
        let google = Google { credentials };
        Ok((Self::with_dialect(google, http, &endpoint, &bucket), prefix))
    }
}

/// `gs://<bucket>/<prefix>` as the bucket and the prefix.
fn parse_url(url: &str) -> Result<(String, String)> {
    let rest = url
        .strip_prefix("gs://")
        .ok_or_else(|| Error::Config(format!("{url}: expected gs://<bucket>/<prefix>")))?;
    let (bucket, prefix) = rest.split_once('/').unwrap_or((rest, ""));
    if bucket.is_empty() {
        return Err(Error::Config(format!(
            "{url}: expected a bucket name in gs://<bucket>/<prefix>"
        )));
    }
    Ok((bucket.to_owned(), prefix.to_owned()))
}

impl Dialect for Google {
    const METADATA: &'static str = "x-goog-meta-";
    const VERSION: &'static str = "x-goog-generation";

    /// A generation that is not a number is none.
    fn version(response: &HttpResponse) -> &str {
        let generation = &response.generation;
        if generation.bytes().all(|b| b.is_ascii_digit()) {
            generation
        } else {
            ""
        }
    }

    /// A version from this store is a generation, which is never 0.
    fn precondition(guard: &Guard) -> std::result::Result<(String, String), String> {
        let generation = match guard {
            Guard::Absent => "0",
            Guard::Match(Version(generation)) => generation,
        };
        let valid = !generation.is_empty()
            && generation.bytes().all(|b| b.is_ascii_digit())
            && (matches!(guard, Guard::Absent) || generation.bytes().any(|b| b != b'0'));
        if !valid {
            return Err(format!("{generation:?} is not a Cloud Storage generation"));
        }
        Ok(("x-goog-if-generation-match".into(), generation.to_owned()))
    }

    async fn authorize(&self, request: &mut Request<'_>) -> Result<Option<String>> {
        let Some(credentials) = &self.credentials else {
            return Ok(None);
        };
        let token = credentials.token().await?;
        let header = format!("Bearer {token}");
        request.headers.push(("authorization".into(), header));
        Ok(Some(token))
    }

    fn refused(&self, token: &str) -> bool {
        self.credentials.as_ref().is_some_and(|c| c.forget(token))
    }
}

#[cfg(test)]
#[path = "gcs-test.rs"]
mod tests;
