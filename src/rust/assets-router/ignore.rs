// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `.assetsignore`: gitignore-style patterns naming files that are not assets. `_headers`,
//! `_redirects` and `.assetsignore` themselves are always ignored.

use regex::Regex;

/// Each pattern: whether it negates, whether it names directories only, and its regex.
pub struct Matcher(Vec<(bool, bool, Regex)>);

impl Matcher {
    /// A pattern that cannot be compiled is skipped with a warning to `warn`.
    pub fn new(patterns: &str, warn: &mut dyn FnMut(String)) -> Self {
        // A byte order mark is not part of the first pattern.
        let patterns = patterns.strip_prefix('\u{feff}').unwrap_or(patterns);
        let lines = ["/.assetsignore", "/_redirects", "/_headers"]
            .into_iter()
            .chain(patterns.split('\n'));
        Self(
            lines
                .filter_map(|line| {
                    let line = trim_end(line.strip_suffix('\r').unwrap_or(line));
                    if line.is_empty() || line.starts_with('#') {
                        return None;
                    }
                    let (negate, line) =
                        line.strip_prefix('!').map_or((false, line), |l| (true, l));
                    let (dir_only, line) =
                        line.strip_suffix('/').map_or((false, line), |l| (true, l));
                    let re = glob_regex(line.strip_prefix('/').unwrap_or(line));
                    let re = if line.contains('/') {
                        format!("^{re}$")
                    } else {
                        format!("^(?:.*/)?{re}$")
                    };
                    let Ok(re) = Regex::new(&re) else {
                        warn(format!(
                            "Skipping the .assetsignore pattern {line}: it cannot be compiled."
                        ));
                        return None;
                    };
                    Some((negate, dir_only, re))
                })
                .collect(),
        )
    }

    /// Whether `pathname`, relative to the directory and starting with `/`, is ignored. As in
    /// git, a file under an ignored directory is ignored, whatever later patterns say.
    pub fn ignored(&self, pathname: &str) -> bool {
        let segments: Vec<&str> = pathname.split('/').filter(|s| !s.is_empty()).collect();
        (1..=segments.len()).any(|i| {
            let (path, dir) = (segments[..i].join("/"), i < segments.len());
            self.0
                .iter()
                .fold(false, |ignored, (negate, dir_only, regex)| {
                    if (!dir_only || dir) && regex.is_match(&path) {
                        !negate
                    } else {
                        ignored
                    }
                })
        })
    }
}

/// `line` without trailing whitespace, but for one whitespace character a backslash escapes.
fn trim_end(line: &str) -> &str {
    let trimmed = line.trim_end();
    let escaped = line[trimmed.len()..]
        .chars()
        .next()
        .filter(|_| trimmed.ends_with('\\'));
    &line[..trimmed.len() + escaped.map_or(0, char::len_utf8)]
}

fn glob_regex(pattern: &str) -> String {
    let p: Vec<char> = pattern.chars().collect();
    let escape = |c: char| regex::escape(c.encode_utf8(&mut [0; 4]));
    let mut re = String::new();
    let mut i = 0;
    while i < p.len() {
        let end = p.iter().skip(i + 2).position(|&c| c == ']');
        match (p[i], end.map(|end| i + 2 + end)) {
            ('*', _) if p.get(i + 1) == Some(&'*') => {
                i += 1;
                if p.get(i + 1) == Some(&'/') {
                    i += 1;
                    re += "(?:.*/)?";
                } else {
                    re += ".*";
                }
            }
            ('*', _) => re += "[^/]*",
            ('?', _) => re += "[^/]",
            ('[', Some(end)) => {
                let body: String = p[i + 1..end].iter().collect();
                let body = body
                    .strip_prefix('!')
                    .map_or_else(|| body.clone(), |b| format!("^{b}"));
                re += &format!("[{}]", body.replace('\\', "\\\\").replace('[', "\\["));
                i = end;
            }
            ('\\', _) if i + 1 < p.len() => {
                i += 1;
                re += &escape(p[i]);
            }
            (c, _) => re += &escape(c),
        }
        i += 1;
    }
    re
}

#[cfg(test)]
#[path = "ignore-test.rs"]
mod tests;
