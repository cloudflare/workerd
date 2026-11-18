// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `base_path` under which assets are served.
//!
//! Ported from workers-sdk's workers-shared, `utils/base-path.ts` and
//! `asset-worker/src/utils/base-path.ts`.

/// The root `/`, or a pathname with a leading and a trailing slash, such as `/subpath/`.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct BasePath(String);

impl Default for BasePath {
    fn default() -> Self {
        Self("/".to_owned())
    }
}

impl BasePath {
    /// Mirrors `normalizeBasePath`, and its messages.
    pub fn normalize(value: &str) -> Result<Self, &'static str> {
        let (scheme, _) = value.split_once(':').unwrap_or_default();
        let mut scheme_chars = scheme.chars();
        let has_scheme = scheme_chars.next().is_some_and(|c| c.is_ascii_alphabetic())
            && scheme_chars.all(|c| c.is_ascii_alphanumeric() || matches!(c, '+' | '.' | '-'));
        for (invalid, message) in [
            (value.is_empty(), "The value must not be empty."),
            (
                value.starts_with("//") || has_scheme,
                "The value must be a pathname, not a URL.",
            ),
            (
                value.contains('\\'),
                "The value must not contain backslashes.",
            ),
            (
                value.chars().any(|c| c <= '\u{1f}' || c == '\u{7f}'),
                "The value must not contain control characters.",
            ),
            (
                value.contains(['?', '#']),
                "The value must not contain query strings or fragments.",
            ),
            (
                value.contains('%'),
                "The value must not contain percent-encoded sequences.",
            ),
        ] {
            if invalid {
                return Err(message);
            }
        }
        let mut segments = Vec::new();
        for segment in value.split('/') {
            match segment {
                "" | "." => {}
                ".." => _ = segments.pop(),
                segment => segments.push(segment),
            }
        }
        if segments.is_empty() {
            return Ok(Self::default());
        }
        Ok(Self(format!("/{}/", segments.join("/"))))
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }

    pub fn is_root(&self) -> bool {
        self.0 == "/"
    }

    /// The base without its trailing slash, e.g. `/subpath`, unless it is the root.
    pub(crate) fn without_trailing_slash(&self) -> Option<&str> {
        self.0.strip_suffix('/').filter(|base| !base.is_empty())
    }

    /// The path under the base of a decoded pathname, matching whole segments. Mirrors
    /// `stripBasePath`.
    pub fn strip<'a>(&self, path: &'a str) -> Option<&'a str> {
        match self.without_trailing_slash() {
            None => Some(path),
            Some(base) if path == base => Some("/"),
            Some(base) => path
                .strip_prefix(self.as_str())
                .map(|_| &path[base.len()..]),
        }
    }

    /// The public path of a path under the base, where `""` is the base without its trailing
    /// slash. Mirrors `addBasePath`.
    pub fn add(&self, path: &str) -> String {
        match self.without_trailing_slash() {
            None => path.to_owned(),
            Some(_) if path == "/" => self.0.clone(),
            Some(base) => format!("{base}{path}"),
        }
    }
}

#[cfg(test)]
#[path = "base_path-test.rs"]
mod tests;
