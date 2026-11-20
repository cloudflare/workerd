// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![forbid(unsafe_code)]

//! Static assets routing, ported from workers-sdk's `packages/workers-shared` at commit
//! dc47e6d2 (2026-11-11); each module names its upstream file.
//!
//! Whether a request goes to the user Worker or to the assets, and what the assets answer. Pure:
//! the host looks files up and sends responses.

use serde::Deserialize;

pub mod base_path;
pub mod decide;
pub mod ignore;
pub mod intent;
pub mod mitigations;
pub mod parse;
pub mod rules;
pub mod serve;

/// The parts of a request that routing looks at.
#[derive(Clone, Copy, Debug)]
pub struct Request<'a> {
    pub url: &'a url::Url,
    pub method: Method,
    pub sec_fetch_mode: Option<&'a str>,
    pub sec_fetch_dest: Option<&'a str>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Method {
    Get,
    Head,
    Other,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResponseHeader {
    pub name: String,
    pub value: String,
}

pub fn header(name: impl Into<String>, value: impl Into<String>) -> ResponseHeader {
    let (name, value) = (name.into(), value.into());
    ResponseHeader { name, value }
}

/// `prefix` and `part`, if `part` is there and not empty.
fn affix(prefix: &str, part: Option<&str>) -> String {
    part.filter(|p| !p.is_empty())
        .map_or_else(String::new, |p| format!("{prefix}{p}"))
}

/// A response sent without running a Worker or reading a file.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Response {
    pub status: u16,
    pub headers: Vec<ResponseHeader>,
    pub body: &'static str,
}

impl Response {
    /// The response that replaces a request or response a mitigation blocks.
    pub fn blocked() -> Self {
        let headers = vec![header("Content-Type", "text/plain;charset=UTF-8")];
        Self {
            status: 403,
            headers,
            body: "Blocked",
        }
    }

    pub fn new(status: u16, headers: Vec<ResponseHeader>) -> Self {
        Self {
            status,
            headers,
            body: "",
        }
    }
}

/// The assets config, every field optional: wrangler's `assets` options, and the text of the
/// directory's `_headers`, `_redirects` and `.assetsignore`.
#[derive(Default, Deserialize)]
#[serde(default)]
pub struct Options {
    pub html_handling: Option<intent::HtmlHandling>,
    pub not_found_handling: Option<intent::NotFoundHandling>,
    pub base_path: Option<String>,
    pub run_worker_first: Option<RunWorkerFirst>,
    pub headers: Option<String>,
    pub redirects: Option<String>,
    pub ignore: Option<String>,
    /// For Miniflare: a header on a navigation that `not_found_handling` answered.
    pub debug: Option<bool>,
}

#[derive(Deserialize)]
#[serde(untagged)]
pub enum RunWorkerFirst {
    All(bool),
    Rules(Vec<String>),
}

pub struct Config {
    pub routing: decide::RoutingConfig,
    pub debug: bool,
    pub ignored: ignore::Matcher,
}

/// What routing needs from the deployment besides its assets config.
#[derive(Clone, Copy, Debug, Default)]
pub struct Deployment {
    pub has_user_worker: bool,
    /// The compiled `assets_navigation_prefers_asset_serving` flag.
    pub navigation_prefers_asset_serving: bool,
}

impl Config {
    /// `warn` gets the warnings about `_headers`, `_redirects` and `.assetsignore`.
    pub fn new(
        options: Options,
        deployment: Deployment,
        warn: &mut dyn FnMut(String),
    ) -> Result<Self, String> {
        let Deployment {
            has_user_worker,
            navigation_prefers_asset_serving,
        } = deployment;
        let (rules, ahead) = match options.run_worker_first {
            Some(RunWorkerFirst::Rules(rules)) => (Some(parse::static_routing(&rules)?), false),
            Some(RunWorkerFirst::All(all)) => (None, all),
            None => (None, false),
        };
        let html_handling = options.html_handling.unwrap_or_default();
        let base_path = match options.base_path {
            Some(path) => base_path::BasePath::normalize(&path)
                .map_err(|e| format!("Invalid assets base_path: {e}"))?,
            None => base_path::BasePath::default(),
        };
        let (static_redirects, dynamic_redirects) = options
            .redirects
            .map(|text| parse::redirects(&text, html_handling, warn))
            .unwrap_or_default();
        let header_rules = options
            .headers
            .map(|text| parse::headers(&text, warn))
            .unwrap_or_default();
        let has_static_routing = rules.is_some();
        let (user, asset) = rules.unwrap_or_default();
        Ok(Self {
            routing: decide::RoutingConfig {
                has_user_worker,
                invoke_user_worker_ahead_of_assets: ahead,
                user_worker_rules: decide::globs(&user),
                asset_worker_rules: decide::globs(&asset),
                intent: intent::IntentConfig {
                    html_handling,
                    not_found_handling: options.not_found_handling.unwrap_or_default(),
                    has_static_routing,
                    navigation_prefers_asset_serving,
                },
                base_path,
                redirects: rules::Redirects::new(static_redirects, dynamic_redirects),
                header_rules: rules::RulesMatcher::new(header_rules),
            },
            debug: options.debug.unwrap_or_default(),
            ignored: ignore::Matcher::new(options.ignore.as_deref().unwrap_or_default(), warn),
        })
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;

#[cfg(test)]
#[path = "golden-test.rs"]
mod golden;
