// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! What a directory of assets answers to a request.
//!
//! Ported from workers-sdk's workers-shared, `asset-worker/src/handler.ts`.

use serde::Deserialize;

use super::Method;
use super::Request;
use super::base_path::BasePath;
use super::rules::Redirect;
use super::rules::resolve_location;

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq)]
#[serde(rename_all = "kebab-case")]
pub enum HtmlHandling {
    #[default]
    AutoTrailingSlash,
    ForceTrailingSlash,
    DropTrailingSlash,
    None,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq)]
#[serde(rename_all = "kebab-case")]
pub enum NotFoundHandling {
    SinglePageApplication,
    #[serde(rename = "404-page")]
    NotFoundPage,
    #[default]
    None,
}

/// A file that answers a request, and how.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Asset {
    /// The file, relative to the directory, starting with `/`.
    pub path: String,
    pub etag: String,
    pub status: u16,
    /// Whether `not_found_handling` chose it.
    pub not_found: bool,
}

type Exists<'a> = &'a dyn Fn(&str) -> Option<Asset>;

/// The asset at a path, from `exists`, which gives a file's `ETag`.
fn found(exists: &impl Fn(&str) -> Option<String>) -> impl Fn(&str) -> Option<Asset> {
    |path| {
        exists(path).map(|etag| Asset {
            path: path.to_owned(),
            etag,
            status: 200,
            not_found: false,
        })
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Intent {
    Asset(Asset),
    /// Redirect to another spelling of the path, relative to the directory.
    Redirect(String),
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct IntentConfig {
    pub html_handling: HtmlHandling,
    pub not_found_handling: NotFoundHandling,
    pub has_static_routing: bool,
    pub navigation_prefers_asset_serving: bool,
}

impl IntentConfig {
    /// The config `canFetch` resolves with: a not-found fallback only counts under static
    /// routing, or for a navigation when the compatibility flag is on.
    pub(crate) fn for_can_fetch(mut self, sec_fetch_mode: Option<&str>) -> Self {
        let navigation = sec_fetch_mode == Some("navigate");
        if !(self.has_static_routing || (self.navigation_prefers_asset_serving && navigation)) {
            self.not_found_handling = NotFoundHandling::None;
        }
        self
    }
}

/// What the assets answer to a request. Mirrors `getResponseOrAssetIntent`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AssetResponse {
    /// Nothing: the user Worker, if any, handles the request.
    NoIntent,
    Redirect {
        status: u16,
        location: String,
    },
    MethodNotAllowed,
    /// A `_redirects` rewrite whose target has no asset.
    NotFound,
    Asset(Asset),
}

/// What the assets answer to `request`, which `redirect` matches.
pub(crate) fn respond(
    request: &Request<'_>,
    redirect: Option<&Redirect>,
    config: IntentConfig,
    base_path: &BasePath,
    exists: &impl Fn(&str) -> Option<String>,
) -> Result<AssetResponse, url::ParseError> {
    let exists = &found(exists);
    // The base applies to the request's own path first, so a rewrite cannot bring a request from
    // outside it into the assets.
    let pathname = request.url.path();
    let decoded = decode_path(pathname);
    if base_path.strip(&decoded).is_none() {
        return Ok(AssetResponse::NoIntent);
    }
    let rewrite = match redirect {
        Some(redirect) if redirect.status == 200 => Some(request.url.join(&redirect.to)?),
        Some(redirect) => {
            return Ok(AssetResponse::Redirect {
                status: match redirect.status {
                    status @ (301 | 303 | 307 | 308) => status,
                    _ => 302,
                },
                location: resolve_location(request.url, &redirect.to)?,
            });
        }
        None => None,
    };
    // A rewrite claims the request: where its target has no asset the answer is a 404.
    let (pathname, decoded, no_intent) = match &rewrite {
        Some(target) => (
            target.path(),
            decode_path(target.path()),
            AssetResponse::NotFound,
        ),
        None => (pathname, decoded, AssetResponse::NoIntent),
    };
    let Some(asset_path) = base_path.strip(&decoded) else {
        return Ok(no_intent);
    };
    let base_root = rewrite
        .is_none()
        .then(|| base_root_intent(&decoded, base_path, config, exists))
        .flatten();
    let Some(intent) = base_root.or_else(|| intent(asset_path, config, exists, false)) else {
        return Ok(no_intent);
    };
    if request.method == Method::Other {
        return Ok(AssetResponse::MethodNotAllowed);
    }
    let (destination, asset) = match intent {
        Intent::Redirect(destination) => (encode_path(&base_path.add(&destination)), None),
        Intent::Asset(asset) => (encode_path(&decoded), Some(asset)),
    };
    match asset.filter(|_| destination == pathname) {
        Some(asset) => Ok(AssetResponse::Asset(asset)),
        None => Ok(AssetResponse::Redirect {
            status: 307,
            location: format!("{destination}{}", super::affix("?", request.url.query())),
        }),
    }
}

/// The redirect from the base root to its spelling under `html_handling`, `/base` to `/base/`
/// or the reverse. Mirrors `getBaseRootIntent`.
fn base_root_intent(
    decoded: &str,
    base_path: &BasePath,
    config: IntentConfig,
    exists: Exists<'_>,
) -> Option<Intent> {
    let destination = match config.html_handling {
        HtmlHandling::AutoTrailingSlash | HtmlHandling::ForceTrailingSlash => {
            (base_path.without_trailing_slash() == Some(decoded)).then_some("/")
        }
        HtmlHandling::DropTrailingSlash => {
            (!base_path.is_root() && decoded == base_path.as_str()).then_some("")
        }
        HtmlHandling::None => None,
    }?;
    exists("/index.html")?;
    Some(Intent::Redirect(destination.to_owned()))
}

pub fn get_intent(
    pathname: &str,
    config: IntentConfig,
    exists: &impl Fn(&str) -> Option<String>,
) -> Option<Intent> {
    intent(pathname, config, &found(exists), false)
}

/// A step of `getIntent`: serve a file, or redirect to a destination if `safeRedirect` allows.
type Step = (String, Option<String>);

fn serve(file: impl Into<String>) -> Step {
    (file.into(), None)
}

fn to(file: impl Into<String>, destination: impl Into<String>) -> Step {
    (file.into(), Some(destination.into()))
}

/// The steps `getIntent` tries for `p` under `html_handling`, in order.
fn steps(p: &str, html_handling: HtmlHandling) -> Vec<Step> {
    use HtmlHandling as H;
    let suffixes = ["/index", "/index.html", "/", ".html"];
    let (suffix, x) = suffixes
        .into_iter()
        .find_map(|suffix| Some((suffix, p.strip_suffix(suffix)?)))
        .unwrap_or_default();
    let (xs, xh, xi) = (
        format!("{x}/"),
        format!("{x}.html"),
        format!("{x}/index.html"),
    );
    let (ps, ph, pi) = (
        format!("{p}/"),
        format!("{p}.html"),
        format!("{p}index.html"),
    );
    let mut steps = match (html_handling, suffix) {
        (H::None, _) => return vec![serve(p)],
        (H::AutoTrailingSlash, "/index") => vec![serve(p), to(&ph, &xs), to(&xh, x)],
        (H::AutoTrailingSlash, "/index.html") => vec![to(p, &xs), to(&xh, x)],
        (H::AutoTrailingSlash, "/") => vec![serve(&pi), to(&xh, x)],
        (H::AutoTrailingSlash, ".html") => vec![to(p, x), to(&xi, &xs)],
        (H::ForceTrailingSlash, "/index") => vec![serve(p), to(&ph, &xs), to(&xh, &xs)],
        (H::ForceTrailingSlash, "/index.html") => vec![to(p, &xs), to(&xh, &xs)],
        (H::ForceTrailingSlash, "/") => vec![serve(&pi), serve(&xh)],
        (H::ForceTrailingSlash, ".html") => vec![to(p, &xs), serve(p), to(&xi, &xs)],
        (H::DropTrailingSlash, "/index") if p == "/index" => vec![serve(p), to("/index.html", "/")],
        (H::DropTrailingSlash, "/index") => vec![serve(p), to(&xh, x), to(&ph, x)],
        (H::DropTrailingSlash, "/index.html") if p == "/index.html" => vec![to(p, "/")],
        (H::DropTrailingSlash, "/index.html") => vec![to(p, x), serve(p), to(&xh, x)],
        (H::DropTrailingSlash, "/") if p == "/" => vec![serve("/index.html")],
        (H::DropTrailingSlash, "/" | ".html") => vec![to(&xh, x), to(&xi, x)],
        _ => vec![],
    };
    let pi_ = format!("{p}/index.html");
    steps.extend(match html_handling {
        H::ForceTrailingSlash => [serve(p), to(&ph, &ps), to(&pi_, &ps)],
        H::AutoTrailingSlash => [serve(p), serve(&ph), to(&pi_, &ps)],
        _ => [serve(p), serve(&ph), serve(&pi_)],
    });
    steps
}

fn intent(pathname: &str, config: IntentConfig, exists: Exists<'_>, skip: bool) -> Option<Intent> {
    steps(pathname, config.html_handling)
        .into_iter()
        .find_map(|(file, destination)| match destination {
            None => exists(&file).map(Intent::Asset),
            Some(destination) => safe_redirect(&file, &destination, config, exists, skip),
        })
        .or_else(|| not_found(pathname, config, exists))
}

/// A redirect to `destination`, if nothing is there and it resolves to an asset with `file`'s
/// `ETag`. Upstream compares only the `ETag`: with content hashes, a copy of `file` elsewhere
/// counts as the same asset.
fn safe_redirect(
    file: &str,
    destination: &str,
    config: IntentConfig,
    exists: Exists<'_>,
    skip: bool,
) -> Option<Intent> {
    if skip || exists(destination).is_some() {
        return None;
    }
    let Some(Intent::Asset(asset)) = intent(destination, config, exists, true) else {
        return None;
    };
    exists(file)
        .filter(|file| file.etag == asset.etag)
        .map(|_| Intent::Redirect(destination.to_owned()))
}

fn not_found(pathname: &str, config: IntentConfig, exists: Exists<'_>) -> Option<Intent> {
    let found = |path: &str, status| {
        let asset = exists(path)?;
        Some(Intent::Asset(Asset {
            status,
            not_found: true,
            ..asset
        }))
    };
    match config.not_found_handling {
        NotFoundHandling::SinglePageApplication => found("/index.html", 200),
        NotFoundHandling::NotFoundPage => {
            let mut dirs = pathname.rmatch_indices('/').map(|(i, _)| &pathname[..i]);
            dirs.find_map(|dir| found(&format!("{dir}/404.html"), 404))
        }
        NotFoundHandling::None => None,
    }
}

/// Mirrors `decodePath`: `decodeURIComponent` on each segment, keeping one it would throw on,
/// then runs of `/` collapsed.
pub fn decode_path(pathname: &str) -> String {
    let decode = |segment: &str| {
        let mut bytes = Vec::new();
        let mut rest = segment.as_bytes();
        while let Some((&b, tail)) = rest.split_first() {
            if b == b'%' {
                let hex = tail
                    .get(..2)
                    .filter(|h| h.iter().all(u8::is_ascii_hexdigit))?;
                let hex = std::str::from_utf8(hex).ok()?;
                bytes.push(u8::from_str_radix(hex, 16).ok()?);
                rest = &tail[2..];
            } else {
                bytes.push(b);
                rest = tail;
            }
        }
        String::from_utf8(bytes).ok()
    };
    let segments: Vec<_> = pathname
        .split('/')
        .map(|s| decode(s).unwrap_or_else(|| s.to_owned()))
        .collect();
    collapse_slashes(&segments.join("/"))
}

/// Mirrors `encodePath`: `encodeURIComponent` on each segment.
pub fn encode_path(pathname: &str) -> String {
    let unreserved = |b: u8| b.is_ascii_alphanumeric() || b"-_.!~*'()".contains(&b);
    let encode = |segment: &str| -> String {
        segment
            .bytes()
            .map(|b| {
                if unreserved(b) {
                    char::from(b).to_string()
                } else {
                    format!("%{b:02X}")
                }
            })
            .collect()
    };
    pathname
        .split('/')
        .map(encode)
        .collect::<Vec<_>>()
        .join("/")
}

pub(crate) fn collapse_slashes(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        if c != '/' || !out.ends_with('/') {
            out.push(c);
        }
    }
    out
}

#[cfg(test)]
#[path = "intent-test.rs"]
mod tests;
