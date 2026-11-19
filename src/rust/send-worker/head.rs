// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Message heads between KJ's types and `http`'s.
//!
//! Headers are copied through kj-hyper's conversions, the ones the server's HTTP front end
//! uses: `kj::HttpHeaders` to an `http::HeaderMap` with the spellings in hyper's
//! `HeaderCaseMap` extension, and back against a `kj::HttpHeaderTable`.

use bytes::Bytes;
use hyper::ext::ReasonPhrase;
use kj::http::HeaderTable;
use kj::http::HeadersRef;
use kj::http::Method;
use kj_hyper::Head;
use kj_hyper::HeaderBlock;
use kj_hyper::ffi::Borrowing;
use kj_hyper::ffi::HttpHeaders;
use kj_hyper::ffi::parse_method;

use crate::Result;

/// `headers` as `http` keeps them, with their spellings in `extensions`.
pub fn headers_from_kj(
    headers: HeadersRef<'_>,
    extensions: &mut http::Extensions,
) -> http::HeaderMap {
    let mut map = http::HeaderMap::new();
    Head::new(headers.as_ffi()).apply(&mut map, extensions);
    map
}

/// `headers` as `kj::HttpHeaders` over `table`, spelled as `extensions` says.
pub fn headers_to_kj<'t>(
    table: &'t HeaderTable,
    headers: &http::HeaderMap,
    extensions: &http::Extensions,
) -> Result<Borrowing<'t, HttpHeaders>> {
    HeaderBlock::new(headers, extensions).to_kj(table)
}

/// The head of a request a `WorkerInterface` caller made.
pub fn request_parts(
    method: Method,
    url: &[u8],
    headers: HeadersRef<'_>,
) -> Result<http::request::Parts> {
    let (mut parts, ()) = http::Request::new(()).into_parts();
    parts.method = method_from_kj(method)?;
    parts.uri = http::Uri::try_from(url)
        .map_err(|e| kj::failed!("the request URL is not one `http::Uri` accepts: {e}"))?;
    parts.headers = headers_from_kj(headers, &mut parts.extensions);
    Ok(parts)
}

fn method_from_kj(method: Method) -> Result<http::Method> {
    // The enumerators are the methods' names as KJ reads and writes them, `MSEARCH` included.
    let name = format!("{method:?}");
    http::Method::from_bytes(name.as_bytes())
        .map_err(|_| kj::failed!("unknown request method {name}"))
}

/// `method` as KJ's enum, which has no place for a method KJ does not know.
pub fn method_to_kj(method: &http::Method) -> Result<Method> {
    let mut parsed = Method::GET;
    if parse_method(method.as_str().as_bytes(), &mut parsed) {
        Ok(parsed)
    } else {
        Err(kj::failed!("request method {method} is not one KJ knows"))
    }
}

/// The reason phrase of a response head: the one it carries, or the status's own.
pub fn reason(parts: &http::response::Parts) -> &str {
    parts
        .extensions
        .get::<ReasonPhrase>()
        .and_then(|reason| std::str::from_utf8(reason.as_bytes()).ok())
        .or_else(|| parts.status.canonical_reason())
        .unwrap_or("")
}

/// The head of a response KJ delivered.
pub fn response_parts(
    status: u32,
    status_text: &str,
    headers: HeadersRef<'_>,
) -> Result<http::response::Parts> {
    let (mut parts, ()) = http::Response::new(()).into_parts();
    parts.status = u16::try_from(status)
        .ok()
        .and_then(|status| http::StatusCode::from_u16(status).ok())
        .ok_or_else(|| kj::failed!("invalid response status {status}"))?;
    parts.headers = headers_from_kj(headers, &mut parts.extensions);
    if parts.status.canonical_reason() != Some(status_text)
        && let Ok(reason) = ReasonPhrase::try_from(Bytes::copy_from_slice(status_text.as_bytes()))
    {
        parts.extensions.insert(reason);
    }
    Ok(parts)
}

#[cfg(test)]
#[path = "head-test.rs"]
mod tests;
