// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The case filter of `workerd bench`: `kj::GlobFilter` (kj/glob-filter.h), so that case names
//! match in Rust exactly as they do in C++.

#[cxx::bridge(namespace = "workerd::api")]
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
mod bridge {
    unsafe extern "C++" {
        include!("workerd/api/bench-core/glob-filter.h");

        /// Whether `name` matches `pattern`, as `kj::GlobFilter(pattern).matches(name)`.
        fn glob_filter_matches(pattern: &[u8], name: &[u8]) -> bool;
    }
}

/// A glob that matches a name, or the part of it after any `/` or `\`.
///
/// `*` matches any run of characters and `?` any one character, but neither matches `/` or `\`.
/// Names and patterns are bytes, compared one byte at a time, as `kj::GlobFilter` compares
/// `char`s.
#[derive(Debug, Clone)]
pub struct GlobFilter {
    pattern: Vec<u8>,
}

impl GlobFilter {
    #[must_use]
    pub fn new(pattern: &[u8]) -> Self {
        Self {
            pattern: pattern.to_vec(),
        }
    }

    /// Whether `name` matches.
    #[must_use]
    pub fn matches(&self, name: &[u8]) -> bool {
        bridge::glob_filter_matches(&self.pattern, name)
    }
}

#[cfg(test)]
#[path = "glob_filter-test.rs"]
mod tests;
