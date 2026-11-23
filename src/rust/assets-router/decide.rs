// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Routing a request between the user Worker and its assets.
//!
//! Ported from workers-sdk's workers-shared, `router-worker/src/worker.ts`.

use regex::Regex;

use super::Request;
use super::Response;
use super::ResponseHeader;
use super::base_path::BasePath;
use super::header;
use super::intent::Asset;
use super::intent::AssetResponse;
use super::intent::IntentConfig;
use super::mitigations;
use super::rules::HeaderRule;
use super::rules::Redirects;
use super::rules::RulesMatcher;
use super::rules::apply_headers;

/// Where the static rules alone send a request.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RuleRoute {
    UserWorker,
    AssetWorker,
    /// The assets handle the request if they have something for it.
    Undecided,
}

#[derive(Debug, Eq, PartialEq)]
pub enum Route {
    /// Run the user Worker, whose response, with `filter_image_response`, must pass
    /// [`mitigations::should_block_image_response`]. With `block_image_href`, the host refuses
    /// the request with [`Response::blocked`] instead, after any checks of its own.
    UserWorker {
        filter_image_response: bool,
        block_image_href: bool,
    },
    Respond(Response),
    Asset(Asset),
}

pub struct RoutingConfig {
    pub has_user_worker: bool,
    pub invoke_user_worker_ahead_of_assets: bool,
    pub user_worker_rules: Vec<Regex>,
    pub asset_worker_rules: Vec<Regex>,
    pub intent: IntentConfig,
    pub base_path: BasePath,
    pub redirects: Redirects,
    pub header_rules: RulesMatcher<HeaderRule>,
}

/// `run_worker_first` rules, in which `*` is the only special character. Mirrors
/// `generateGlobOnlyRuleRegExp`.
pub fn globs(rules: &[String]) -> Vec<Regex> {
    let glob = |rule: &String| {
        rule.split('*')
            .map(regex::escape)
            .collect::<Vec<_>>()
            .join(".*")
    };
    rules
        .iter()
        .filter_map(|rule| Regex::new(&format!("^{}$", glob(rule))).ok())
        .collect()
}

impl RoutingConfig {
    pub fn rule_route(&self, pathname: &str) -> RuleRoute {
        let matches = |rules: &[Regex]| rules.iter().any(|r| r.is_match(pathname));
        if matches(&self.asset_worker_rules) {
            RuleRoute::AssetWorker
        } else if matches(&self.user_worker_rules) || self.invoke_user_worker_ahead_of_assets {
            RuleRoute::UserWorker
        } else {
            RuleRoute::Undecided
        }
    }

    /// Route `request`, which [`Self::rule_route`] sent to `rule_route`. `exists` gives the
    /// `ETag` of a file at a path relative to the directory.
    pub fn route(
        &self,
        rule_route: RuleRoute,
        request: &Request<'_>,
        exists: &impl Fn(&str) -> Option<String>,
    ) -> Result<Route, String> {
        let response = match rule_route {
            RuleRoute::UserWorker => return self.user_worker_route(request),
            RuleRoute::Undecided if self.has_user_worker => {
                let config = self.intent.for_can_fetch(request.sec_fetch_mode);
                let can_fetch = self.asset_response(request, config, exists)?;
                if can_fetch == AssetResponse::NoIntent {
                    return self.user_worker_route(request);
                }
                if config == self.intent {
                    can_fetch
                } else {
                    self.asset_response(request, self.intent, exists)?
                }
            }
            _ => self.asset_response(request, self.intent, exists)?,
        };
        let respond = |status, headers| {
            let headers = self.custom_headers(request, headers);
            Ok(Route::Respond(Response::new(status, headers)))
        };
        match response {
            AssetResponse::Redirect { status, location } => {
                respond(status, vec![header("Location", location)])
            }
            AssetResponse::MethodNotAllowed => respond(405, vec![]),
            AssetResponse::NotFound | AssetResponse::NoIntent => respond(404, vec![]),
            AssetResponse::Asset(asset) => Ok(Route::Asset(asset)),
        }
    }

    fn asset_response(
        &self,
        request: &Request<'_>,
        config: IntentConfig,
        exists: &impl Fn(&str) -> Option<String>,
    ) -> Result<AssetResponse, String> {
        let redirect = self.redirects.find(request.url);
        super::intent::respond(request, redirect.as_ref(), config, &self.base_path, exists)
            .map_err(|e| format!("invalid _redirects target: {e}"))
    }

    /// Mirrors `routeToUserWorker`: a missing user Worker is an error even for a request that
    /// would be blocked.
    fn user_worker_route(&self, request: &Request<'_>) -> Result<Route, String> {
        if !self.has_user_worker {
            return Err("Fetch for user worker without having a user worker binding".into());
        }
        Ok(Route::UserWorker {
            filter_image_response: mitigations::filters_next_image_response(request),
            block_image_href: mitigations::blocks_image_href(request),
        })
    }

    /// `headers`, with the `_headers` rules for `request` applied.
    pub fn custom_headers(
        &self,
        request: &Request<'_>,
        headers: Vec<ResponseHeader>,
    ) -> Vec<ResponseHeader> {
        let host = request.url.host_str().unwrap_or_default();
        apply_headers(&self.header_rules, host, request.url.path(), headers)
    }
}

#[cfg(test)]
#[path = "decide-test.rs"]
mod tests;
