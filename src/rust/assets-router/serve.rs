// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The head of the response that serves an asset, whatever store its body comes from.
//!
//! Ported from workers-sdk's workers-shared, `asset-worker/src/utils/headers.ts`.

use super::Config;
use super::Method;
use super::Request;
use super::ResponseHeader;
use super::header;
use super::intent::Asset;

#[derive(Debug, Eq, PartialEq)]
pub struct Served {
    pub status: u16,
    pub headers: Vec<ResponseHeader>,
    /// Whether the asset's body follows.
    pub body: bool,
}

/// The request headers that serving looks at.
#[derive(Clone, Copy, Debug, Default)]
pub struct Conditions<'a> {
    pub if_none_match: Option<&'a str>,
    /// Whether the request has an `Authorization` or a `Range` header.
    pub private: bool,
}

/// The `Content-Type` of `path`, by its extension, with a charset for text.
pub fn content_type(path: &str) -> Option<String> {
    let extension = path
        .rsplit_once('.')
        .map_or("", |(_, e)| e)
        .to_ascii_lowercase();
    let content_type = match extension.as_str() {
        "html" | "htm" => "text/html",
        "css" => "text/css",
        "js" | "mjs" | "cjs" => "text/javascript",
        "json" | "map" => "application/json",
        "jsonld" => "application/ld+json",
        "webmanifest" => "application/manifest+json",
        "txt" => "text/plain",
        "md" => "text/markdown",
        "csv" => "text/csv",
        "ics" => "text/calendar",
        "xml" => "application/xml",
        "rss" => "application/rss+xml",
        "atom" => "application/atom+xml",
        "svg" => "image/svg+xml",
        "png" => "image/png",
        "apng" => "image/apng",
        "jpg" | "jpeg" => "image/jpeg",
        "gif" => "image/gif",
        "webp" => "image/webp",
        "avif" => "image/avif",
        "bmp" => "image/bmp",
        "ico" => "image/vnd.microsoft.icon",
        "woff" => "font/woff",
        "woff2" => "font/woff2",
        "ttf" => "font/ttf",
        "otf" => "font/otf",
        "eot" => "application/vnd.ms-fontobject",
        "wasm" => "application/wasm",
        "pdf" => "application/pdf",
        "zip" => "application/zip",
        "gz" => "application/gzip",
        "tar" => "application/x-tar",
        "mp3" => "audio/mpeg",
        "m4a" => "audio/mp4",
        "wav" => "audio/wav",
        "ogg" | "oga" => "audio/ogg",
        "flac" => "audio/flac",
        "mp4" => "video/mp4",
        "webm" => "video/webm",
        "ogv" => "video/ogg",
        "mov" => "video/quicktime",
        _ => return None,
    };
    Some(if content_type.starts_with("text/") {
        format!("{content_type}; charset=utf-8")
    } else {
        content_type.to_owned()
    })
}

impl Config {
    /// The response to `request` that serves `asset`, whose `ETag` is now `etag`.
    pub fn serve(
        &self,
        request: &Request<'_>,
        conditions: Conditions<'_>,
        asset: &Asset,
        etag: &str,
    ) -> Served {
        let etag = format!("\"{etag}\"");
        let mut headers = vec![header("ETag", etag.clone())];
        headers.extend(content_type(&asset.path).map(|t| header("Content-Type", t)));
        if !conditions.private {
            headers.push(header(
                "Cache-Control",
                "public, max-age=0, must-revalidate",
            ));
        }
        if self.debug && asset.not_found && self.routing.intent.navigation_prefers_asset_serving {
            let value = "`Sec-Fetch-Mode: navigate` header present - using `not_found_handling` \
                         behavior";
            headers.push(header("X-Mf-Additional-Response-Log", value));
        }
        let headers = self.routing.custom_headers(request, headers);
        let if_none_match = conditions.if_none_match.unwrap_or_default();
        let (status, body) =
            if if_none_match == etag || if_none_match.strip_prefix("W/") == Some(&etag) {
                (304, false)
            } else {
                (asset.status, request.method != Method::Head)
            };
        Served {
            status,
            headers,
            body,
        }
    }
}

#[cfg(test)]
#[path = "serve-test.rs"]
mod tests;
