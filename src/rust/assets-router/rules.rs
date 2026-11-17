// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `_redirects` and `_headers` rule matching.
//!
//! Ported from workers-sdk's workers-shared, `asset-worker/src/utils/rules-engine.ts`. Relative
//! rules match the pathname; `https://` rules match hostname and pathname, ignoring the port.

use std::collections::HashMap;
use std::collections::HashSet;

use regex::Regex;
use url::Url;

use super::ResponseHeader;
use super::affix;
use super::header;
use super::intent::collapse_slashes;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Redirect {
    pub status: u16,
    pub to: String,
}

#[derive(Default)]
pub struct HeaderRule {
    pub set: Vec<(String, String)>,
    pub unset: Vec<String>,
}

/// Rules in order, each a regex over the pathname, or over `https://host/path` for a rule with
/// a host, and its value.
pub struct RulesMatcher<T>(Vec<(bool, Regex, T)>);

impl<T> RulesMatcher<T> {
    pub fn new(rules: impl IntoIterator<Item = (String, T)>) -> Self {
        Self(
            rules
                .into_iter()
                .filter_map(|(rule, value)| {
                    Some((rule.starts_with("https://"), compile(&rule)?, value))
                })
                .collect(),
        )
    }

    /// Every matching rule's value, with its placeholders' values.
    pub fn matches<'a>(
        &'a self,
        hostname: &str,
        pathname: &'a str,
    ) -> impl Iterator<Item = (&'a T, Vec<(String, String)>)> {
        let absolute = format!("https://{hostname}{pathname}");
        self.0.iter().filter_map(move |(cross_host, regex, value)| {
            let captures = regex.captures(if *cross_host { &absolute } else { pathname })?;
            let names = regex.capture_names().flatten();
            let values = names.filter_map(|n| Some((n.into(), captures.name(n)?.as_str().into())));
            Some((value, values.collect()))
        })
    }
}

/// Mirrors `generateRuleRegExp`: `*` is a splat, `:name` a placeholder, which in the host does
/// not match a `.`.
pub(crate) fn compile(rule: &str) -> Option<Regex> {
    let tokens = Regex::new(r"\*|:[A-Za-z][A-Za-z0-9_]*").ok()?;
    let host_end = rule
        .strip_prefix("https://")
        .and_then(|rest| rest.find('/'))
        .map_or(0, |i| i + "https://".len());
    let (mut pattern, mut last) = (String::from("^"), 0);
    for token in tokens.find_iter(rule) {
        pattern += &regex::escape(&rule[last..token.start()]);
        pattern += &match token.as_str() {
            "*" => "(?P<splat>.*)".to_owned(),
            name if token.start() < host_end => format!("(?P<{}>[^/.]+)", &name[1..]),
            name => format!("(?P<{}>[^/]+)", &name[1..]),
        };
        last = token.end();
    }
    Regex::new(&format!("{pattern}{}$", regex::escape(&rule[last..]))).ok()
}

pub fn replacer(value: &str, replacements: &[(String, String)]) -> String {
    replacements
        .iter()
        .fold(value.to_owned(), |value, (name, replacement)| {
            value.replace(&format!(":{name}"), replacement)
        })
}

/// `headers`, with the `_headers` rules that match applied.
pub fn apply_headers(
    rules: &RulesMatcher<HeaderRule>,
    hostname: &str,
    pathname: &str,
    mut headers: Vec<ResponseHeader>,
) -> Vec<ResponseHeader> {
    let mut set = HashSet::new();
    for (rule, replacements) in rules.matches(hostname, pathname) {
        for name in &rule.unset {
            headers.retain(|h| !h.name.eq_ignore_ascii_case(name));
        }
        for (name, value) in &rule.set {
            if set.insert(name.to_ascii_lowercase()) {
                headers.retain(|h| !h.name.eq_ignore_ascii_case(name));
            }
            headers.push(header(name, replacer(value, &replacements)));
        }
    }
    headers
}

pub fn resolve_location(request: &Url, target: &str) -> Result<String, url::ParseError> {
    let mut to = request.join(target)?;
    if to.query().is_none_or(str::is_empty) {
        to.set_query(request.query());
    }
    if to.origin() != request.origin() {
        return Ok(to.into());
    }
    Ok(format!(
        "{}{}{}",
        to.path(),
        affix("?", to.query()),
        affix("#", to.fragment())
    ))
}

/// A deployment's `_redirects` rules: static ones by path, with their line numbers, and dynamic
/// ones.
pub struct Redirects {
    static_rules: HashMap<String, (usize, Redirect)>,
    dynamic_rules: RulesMatcher<Redirect>,
}

impl Redirects {
    pub fn new(
        static_rules: impl IntoIterator<Item = (String, (usize, Redirect))>,
        dynamic_rules: impl IntoIterator<Item = (String, Redirect)>,
    ) -> Self {
        Self {
            static_rules: static_rules.into_iter().collect(),
            dynamic_rules: RulesMatcher::new(dynamic_rules),
        }
    }

    /// The rule for a request for `url`: a static rule, the first declared of one with the host
    /// and one without, else the first dynamic rule.
    pub fn find(&self, url: &Url) -> Option<Redirect> {
        let (hostname, pathname) = (url.host_str().unwrap_or_default(), url.path());
        let port = url.port().map(|port| port.to_string());
        let with_host = format!(
            "https://{hostname}{}{pathname}",
            affix(":", port.as_deref())
        );
        let static_rule = [with_host.as_str(), pathname]
            .into_iter()
            .filter_map(|key| self.static_rules.get(key))
            .min_by_key(|(line, _)| *line);
        if let Some((_, redirect)) = static_rule {
            return Some(redirect.clone());
        }
        let (redirect, replacements) = self.dynamic_rules.matches(hostname, pathname).next()?;
        let to = replacer(&redirect.to, &replacements).trim().to_owned();
        let scheme = to.split_once("://").map(|(scheme, _)| scheme);
        let has_scheme = scheme.is_some_and(|s| {
            !s.is_empty() && s.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_')
        });
        Some(Redirect {
            status: redirect.status,
            to: if has_scheme {
                to
            } else {
                collapse_slashes(&to)
            },
        })
    }
}

#[cfg(test)]
#[path = "rules-test.rs"]
mod tests;
