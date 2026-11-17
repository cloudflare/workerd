// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Parsing `_headers`, `_redirects` and `run_worker_first`.
//!
//! Ported from workers-sdk's workers-shared, `utils/configuration/`, with its messages.

use std::collections::HashSet;

use url::Url;

use super::affix;
use super::intent::HtmlHandling;
use super::rules::HeaderRule;
use super::rules::Redirect;
use super::rules::compile;

const MAX_LINE_LENGTH: usize = 2000;
const MAX_HEADER_RULES: usize = 100;
const MAX_DYNAMIC_REDIRECT_RULES: usize = 100;
const MAX_STATIC_REDIRECT_RULES: usize = 2000;
const MAX_ROUTES_RULES: usize = 100;
const MAX_ROUTES_RULE_LENGTH: usize = 100;

/// A line that was skipped, and why.
#[derive(Debug, Eq, PartialEq)]
pub struct Invalid {
    pub line: Option<String>,
    pub line_number: Option<usize>,
    pub message: String,
}

impl Invalid {
    fn new(message: String) -> Self {
        Self {
            line: None,
            line_number: None,
            message,
        }
    }

    fn at(line: &str, line_number: usize, message: impl Into<String>) -> Self {
        Self {
            line: Some(line.to_owned()),
            line_number: Some(line_number),
            message: message.into(),
        }
    }
}

/// `path` as a URL normalizes it, with its query and fragment for a `target`.
fn extract_pathname(path: &str, target: bool) -> Option<String> {
    let slash = if path.starts_with('/') { "" } else { "/" };
    let url = Url::parse(&format!("relative://{slash}{path}")).ok()?;
    let (query, fragment) = (
        url.query().filter(|_| target),
        url.fragment().filter(|_| target),
    );
    Some(format!(
        "{}{}{}",
        url.path(),
        affix("?", query),
        affix("#", fragment)
    ))
}

/// The host and path of an `https://` URL.
fn url_host(token: &str) -> Option<(&str, &str)> {
    let rest = token.strip_prefix("https://")?.trim_start_matches('/');
    let (host, path) = rest.split_once('/').unwrap_or((rest, ""));
    (!host.is_empty()).then_some((host, path))
}

/// Mirrors `validateUrl`. A `target`, a redirect's, may have a port, a query and a fragment.
pub fn validate_url(token: &str, only_relative: bool, target: bool) -> Result<String, String> {
    if let Some((host, path)) = url_host(token) {
        if only_relative {
            return Err(format!(
                "Only relative URLs are allowed. Skipping absolute URL {token}."
            ));
        }
        let port = host.rsplit_once(':').map(|(_, port)| port);
        if !target && port.is_some_and(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit())) {
            return Err(format!(
                "Specifying ports is not supported. Skipping absolute URL {token}."
            ));
        }
        return extract_pathname(path, target)
            .map(|path| format!("https://{host}{path}"))
            .ok_or_else(|| format!("Error parsing URL segment {token}. Skipping."));
    }
    let token = if token.starts_with('/') || !only_relative {
        token.to_owned()
    } else {
        format!("/{token}")
    };
    if token.starts_with('/') {
        return extract_pathname(&token, target)
            .ok_or_else(|| format!("Error parsing URL segment {token}. Skipping."));
    }
    Err(if only_relative {
        "URLs should begin with a forward-slash.".into()
    } else {
        "URLs should either be relative (e.g. begin with a forward-slash), or use HTTPS (e.g. \
         begin with \"https://\")."
            .into()
    })
}

/// Whether `path` has a `:placeholder`.
fn has_placeholder(path: &str) -> bool {
    path.match_indices(':')
        .any(|(i, _)| path[i + 1..].starts_with(|c: char| c.is_ascii_alphabetic()))
}

/// Mirrors `parseHeaders`.
pub fn parse_headers(input: &str, max_rules: usize) -> (Vec<(String, HeaderRule)>, Vec<Invalid>) {
    let lines: Vec<&str> = input.split('\n').collect();
    let (mut rules, mut invalid) = (Vec::new(), Vec::new());
    let mut rule: Option<(String, HeaderRule, &str)> = None;
    let mut skip_until_next_path = false;
    for (i, line) in lines.iter().enumerate() {
        let (line, n) = (line.trim(), i + 1);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if line.len() > MAX_LINE_LENGTH {
            invalid.push(Invalid::new(format!(
                "Ignoring line {n} as it exceeds the maximum allowed length of {MAX_LINE_LENGTH}."
            )));
            continue;
        }
        let first = line
            .split_once(char::is_whitespace)
            .map_or(line, |(first, _)| first);
        if line.starts_with('/') || first.find("://").is_some_and(|i| i > 0) {
            skip_until_next_path = false;
            if rules.len() >= max_rules {
                invalid.push(Invalid::new(format!(
                    "Maximum number of rules supported is {max_rules}. Skipping remaining {} \
                     lines of file.",
                    lines.len() - i
                )));
                break;
            }
            if let Some(rule) = rule.take() {
                push(&mut rules, &mut invalid, rule, Some(n));
            }
            match validate_url(line, false, false)
                .and_then(|path| no_multiple_wildcards(&path).map(|()| path))
            {
                Ok(path) => rule = Some((path, HeaderRule::default(), line)),
                Err(message) => {
                    invalid.push(Invalid::at(line, n, message));
                    skip_until_next_path = true;
                }
            }
            continue;
        }
        if skip_until_next_path {
            continue;
        }
        let Some((name, value)) = line.split_once(':') else {
            match &mut rule {
                None => invalid.push(Invalid::at(
                    line,
                    n,
                    "Expected a path beginning with at least one forward-slash",
                )),
                Some((_, rule, _)) if line.starts_with("! ") => rule.unset.push(line[2..].into()),
                Some(_) => invalid.push(Invalid::at(
                    line,
                    n,
                    "Expected a colon-separated header pair (e.g. name: value)",
                )),
            }
            continue;
        };
        let (name, value) = (name.trim().to_lowercase(), value.trim());
        let message = if name.contains(' ') {
            "Header name cannot include spaces".into()
        } else if name.is_empty() {
            "No header name specified".into()
        } else if !name.bytes().all(is_token_char) {
            // Upstream accepts these, and setting the header then fails every response.
            "Header name can only include token characters".into()
        } else if value.is_empty() {
            "No header value specified".into()
        } else if let Some((_, rule, _)) = &mut rule {
            match rule.set.iter_mut().find(|(n, _)| *n == name) {
                Some((_, existing)) => *existing = format!("{existing}, {value}"),
                None => rule.set.push((name, value.into())),
            }
            continue;
        } else {
            format!("Path should come before header ({name}: {value})")
        };
        invalid.push(Invalid::at(line, n, message));
    }
    if let Some(rule) = rule {
        push(&mut rules, &mut invalid, rule, None);
    }
    (rules, invalid)
}

fn push(
    rules: &mut Vec<(String, HeaderRule)>,
    invalid: &mut Vec<Invalid>,
    (path, rule, line): (String, HeaderRule, &str),
    line_number: Option<usize>,
) {
    if rule.set.is_empty() && rule.unset.is_empty() {
        let line = Some(line.to_owned());
        invalid.push(Invalid {
            line,
            line_number,
            message: "No headers specified".into(),
        });
    } else {
        rules.push((path, rule));
    }
}

fn no_multiple_wildcards(path: &str) -> Result<(), String> {
    let wildcards = path.matches('*').count();
    if wildcards > 1 {
        return Err(format!(
            "Only one wildcard is allowed per rule. Use a named placeholder (e.g. :project) \
             instead. Skipping {path}."
        ));
    }
    let splat = path
        .match_indices(":splat")
        .any(|(i, _)| !path[i + 6..].starts_with(|c: char| c.is_ascii_alphanumeric() || c == '_'));
    if wildcards > 0 && splat {
        return Err(format!(
            "Cannot combine a wildcard * with a :splat placeholder because wildcards are \
             converted to :splat at runtime. Skipping {path}."
        ));
    }
    Ok(())
}

/// Mirrors `parseRedirects`.
pub fn parse_redirects(
    input: &str,
    html_handling: HtmlHandling,
    max_static_rules: usize,
    max_dynamic_rules: usize,
) -> (Vec<(String, usize, Redirect)>, Vec<Invalid>) {
    let lines: Vec<&str> = input.split('\n').collect();
    let (mut rules, mut seen, mut invalid) = (Vec::new(), HashSet::new(), Vec::new());
    let (mut static_rules, mut dynamic_rules) = (0, 0);
    let mut can_create_static_rule = true;
    for (i, line) in lines.iter().enumerate() {
        let (line, n) = (line.trim(), i + 1);
        let fail = |message| Invalid::at(line, n, message);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if line.len() > MAX_LINE_LENGTH {
            invalid.push(Invalid::new(format!(
                "Ignoring line {n} as it exceeds the maximum allowed length of {MAX_LINE_LENGTH}."
            )));
            continue;
        }
        // An inline comment starts with whitespace and `#`.
        let comment = line
            .match_indices('#')
            .find(|(i, _)| line[..*i].ends_with(char::is_whitespace));
        let tokens: Vec<&str> = line[..comment.map_or(line.len(), |(i, _)| i)]
            .split_whitespace()
            .collect();
        if !(2..=3).contains(&tokens.len()) {
            invalid.push(fail(format!(
                "Expected exactly 2 or 3 whitespace-separated tokens. Got {}.",
                tokens.len()
            )));
            continue;
        }
        let status = tokens.get(2).copied().unwrap_or("302");
        let from = match validate_url(tokens[0], true, false) {
            Ok(from) => from,
            Err(message) => {
                invalid.push(fail(message));
                continue;
            }
        };
        if can_create_static_rule && !from.contains('*') && !has_placeholder(&from) {
            static_rules += 1;
            if static_rules > max_static_rules {
                invalid.push(Invalid::new(format!(
                    "Maximum number of static rules supported is {max_static_rules}. Skipping \
                     line."
                )));
                continue;
            }
        } else {
            dynamic_rules += 1;
            can_create_static_rule = false;
            if dynamic_rules > max_dynamic_rules {
                invalid.push(Invalid::new(format!(
                    "Maximum number of dynamic rules supported is {max_dynamic_rules}. Skipping \
                     remaining {} lines of file.",
                    lines.len() - i
                )));
                break;
            }
        }
        let to = match validate_url(tokens[1], false, true) {
            Ok(to) => to,
            Err(message) => {
                invalid.push(fail(message));
                continue;
            }
        };
        let Some(status) = status
            .parse()
            .ok()
            .filter(|s| [200, 301, 302, 303, 307, 308].contains(s))
        else {
            invalid.push(fail(format!(
                "Valid status codes are 200, 301, 302 (default), 303, 307, or 308. Got {status}."
            )));
            continue;
        };
        let to_index = to.ends_with("/index") || to.ends_with("/index.html");
        let absolute = url_host(&to).is_some();
        if !absolute
            && to_index
            && (from.ends_with("/*")
                || (from.ends_with('/') && html_handling != HtmlHandling::None))
        {
            invalid.push(fail(
                "Infinite loop detected in this rule and has been ignored. This will cause a \
                 redirect to strip `.html` or `/index` and end up triggering this rule again. \
                 Please fix or remove this rule to silence this warning."
                    .into(),
            ));
            continue;
        }
        if !seen.insert(from.clone()) {
            invalid.push(fail(format!("Ignoring duplicate rule for path {from}.")));
            continue;
        }
        if status == 200 && absolute {
            invalid.push(fail(format!(
                "Proxy (200) redirects can only point to relative paths. Got {to}"
            )));
            continue;
        }
        rules.push((from, n, Redirect { status, to }));
    }
    (rules, invalid)
}

/// Mirrors `parseStaticRouting`: the user Worker's rules and the assets'.
pub fn static_routing(input: &[String]) -> Result<(Vec<String>, Vec<String>), String> {
    if input.is_empty() {
        return Err(
            "No `run_worker_first` rules were provided; must provide at least 1 rule.".into(),
        );
    }
    if input.len() > MAX_ROUTES_RULES {
        let distinct = input.iter().collect::<HashSet<_>>().len();
        let distinct = if distinct < input.len() {
            format!("({distinct} distinct) ")
        } else {
            String::new()
        };
        return Err(format!(
            "Too many `run_worker_first` rules were provided; {} rules provided {distinct}exceeds \
             max of {MAX_ROUTES_RULES}.",
            input.len()
        ));
    }
    let (mut raw_asset, mut asset, mut user, mut errors) = (vec![], vec![], vec![], vec![]);
    for rule in input {
        if let Some(path) = rule.strip_prefix('!').filter(|path| path.starts_with('/')) {
            asset.push(path.to_owned());
            raw_asset.push(rule.clone());
        } else if rule.starts_with('/') {
            user.push(rule.clone());
        } else if rule.starts_with('!') {
            errors.push(format!("'{rule}': negative rules must start with '!/'"));
        } else {
            errors.push(format!("'{rule}': rules must start with '/' or '!/'"));
        }
    }
    if !asset.is_empty() && user.is_empty() {
        return Err(
            "Only negative `run_worker_first` rules were provided; must provide at least \
                    1 non-negative rule"
                .into(),
        );
    }
    errors.extend(validate_static_routing_rules(&user));
    errors.extend(validate_static_routing_rules(&raw_asset));
    if !errors.is_empty() {
        return Err(format!(
            "Invalid routes in `run_worker_first`:\n{}",
            errors.join("\n")
        ));
    }
    Ok((user, asset))
}

fn validate_static_routing_rules(rules: &[String]) -> Vec<String> {
    let (mut invalid, mut seen) = (Vec::new(), HashSet::new());
    for rule in rules {
        if rule.len() > MAX_ROUTES_RULE_LENGTH {
            invalid.push(format!(
                "'{rule}': all rules must be less than {MAX_ROUTES_RULE_LENGTH} characters in \
                 length"
            ));
        }
        if !seen.insert(rule) {
            invalid.push(format!(
                "'{rule}': rule is a duplicate; rules must be unique"
            ));
        }
        if let Some(prefix) = rule.strip_suffix('*') {
            for other in rules {
                if other != rule && other.starts_with(prefix) {
                    invalid.push(format!("'{other}': rule '{rule}' makes it redundant"));
                }
            }
        }
    }
    invalid
}

/// Mirrors `warnInvalid`.
fn warn_invalid(warn: &mut dyn FnMut(String), kind: &str, file: &str, invalid: &[Invalid]) {
    if invalid.is_empty() {
        return;
    }
    let list = invalid.iter().map(
        |Invalid {
             line,
             line_number,
             message,
         }| match line {
            Some(line) => {
                let n = line_number.map(|n| format!(":{n}")).unwrap_or_default();
                format!("  {message}\n    at {file}{n} | {line}")
            }
            None => format!("  {message}"),
        },
    );
    warn(format!(
        "Found {} invalid {kind} rule(s):\n{}",
        invalid.len(),
        list.collect::<Vec<_>>().join("\n")
    ));
}

type Redirects = (Vec<(String, (usize, Redirect))>, Vec<(String, Redirect)>);

/// The static and dynamic rules of `_redirects`. Mirrors `constructRedirects`.
pub fn redirects(
    text: &str,
    html_handling: HtmlHandling,
    warn: &mut dyn FnMut(String),
) -> Redirects {
    let (rules, invalid) = parse_redirects(
        text,
        html_handling,
        MAX_STATIC_REDIRECT_RULES,
        MAX_DYNAMIC_REDIRECT_RULES,
    );
    warn_invalid(warn, "redirect", "_redirects", &invalid);
    let (mut static_rules, mut dynamic_rules) = (Vec::new(), Vec::new());
    for (from, line_number, redirect) in rules {
        if dynamic_rules.is_empty() && !from.contains('*') && !has_placeholder(&from) {
            static_rules.push((from, (line_number, redirect)));
        } else {
            dynamic_rules.push((from, redirect));
        }
    }
    dynamic_rules.retain(|(from, _)| compiles(from, "_redirects", warn));
    (static_rules, dynamic_rules)
}

/// Whether `rule` compiles to a matcher; a rule that does not, such as one repeating a
/// placeholder, is skipped with a warning.
fn compiles(rule: &str, file: &str, warn: &mut dyn FnMut(String)) -> bool {
    let ok = compile(rule).is_some();
    if !ok {
        warn(format!(
            "Skipping the {file} rule {rule}: it cannot be compiled."
        ));
    }
    ok
}

/// An HTTP token character (RFC 9110 `tchar`).
const fn is_token_char(b: u8) -> bool {
    b.is_ascii_alphanumeric()
        || matches!(
            b,
            b'!' | b'#'
                | b'$'
                | b'%'
                | b'&'
                | b'\''
                | b'*'
                | b'+'
                | b'-'
                | b'.'
                | b'^'
                | b'_'
                | b'`'
                | b'|'
                | b'~'
        )
}

/// The rules of `_headers`. Mirrors `constructHeaders`.
pub fn headers(text: &str, warn: &mut dyn FnMut(String)) -> Vec<(String, HeaderRule)> {
    let (rules, invalid) = parse_headers(text, MAX_HEADER_RULES);
    warn_invalid(warn, "header", "_headers", &invalid);
    let mut out: Vec<(String, HeaderRule)> = Vec::new();
    for (path, rule) in rules {
        match out.iter_mut().find(|(p, _)| *p == path) {
            Some((_, existing)) => *existing = rule,
            None => out.push((path, rule)),
        }
    }
    out.retain(|(path, _)| compiles(path, "_headers", warn));
    out
}

#[cfg(test)]
#[path = "parse-test.rs"]
mod tests;
