// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Security mitigations around assets routing.
//!
//! Ported from workers-sdk's workers-shared, `router-worker/src/worker.ts`.

use std::borrow::Cow;

use url::Url;

use super::Method;
use super::Request;

/// Whether URL parsing alone made this a `/cdn-cgi/` request, from a raw `/cdn-cgi\`.
///
/// Such a request slipped past the routing of `/cdn-cgi/` ahead of Workers, so it is redirected
/// to its parsed URL before any routing.
pub fn is_cdn_cgi_backslash_bypass(raw_url: &str, url: &Url) -> bool {
    url.path().starts_with("/cdn-cgi/") && raw_url.contains("/cdn-cgi\\")
}

/// Whether to refuse `/_image?href=//other-host/...`, unless the browser asks for an image.
pub(crate) fn blocks_image_href(request: &Request<'_>) -> bool {
    let other_host = |href: Cow<'_, str>| {
        href.len() > 2
            && href.starts_with("//")
            && Url::parse(&format!("https:{href}"))
                .is_ok_and(|href| href.host_str() != request.url.host_str())
    };
    request.url.path() == "/_image"
        && query_param(request.url, "href").is_some_and(other_host)
        && request.sec_fetch_dest != Some("image")
}

/// Whether the user Worker's response must pass [`should_block_image_response`]: a request for
/// `*/_next/image?url=<remote>` that is not the browser's for an image.
pub(crate) fn filters_next_image_response(request: &Request<'_>) -> bool {
    request.url.path().ends_with("/_next/image")
        && query_param(request.url, "url").is_some_and(|s| !s.is_empty() && !s.starts_with('/'))
        && (request.method != Method::Get || request.sec_fetch_dest != Some("image"))
}

/// Whether a filtered response is replaced by a 403: anything but a 304 must have one
/// `Content-Type`, an image type or plain text.
pub fn should_block_image_response(status: u16, content_type: &[u8]) -> bool {
    let Ok(content_type) = std::str::from_utf8(content_type) else {
        return status != 304;
    };
    status != 304
        && (content_type.is_empty()
            || content_type.contains(',')
            || !(content_type.starts_with("image/")
                || content_type.split(';').next() == Some("text/plain")))
}

/// What `URLSearchParams.get(name)` returns.
fn query_param<'a>(url: &'a Url, name: &str) -> Option<Cow<'a, str>> {
    url.query_pairs()
        .find(|(key, _)| key == name)
        .map(|(_, value)| value)
}

#[cfg(test)]
#[path = "mitigations-test.rs"]
mod tests;
