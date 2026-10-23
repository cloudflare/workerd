// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `HttpOptions` of a socket or an external server applied to the traffic through it.
//!
//! The options are the URL style (proxy-form URLs with the host inside, or origin-form URLs with
//! a `Host` header), the cf blob header, injected headers, and the host that carries capnp RPC
//! over HTTP CONNECT.
//!
//! Headers stay `kj::HttpHeaders` throughout. Every header the options name is in the factory's
//! header table, so the rewriter says what to set or remove by name (`HeaderEdit`) and the
//! factory applies it by table id to a copy; kj then writes those headers where it writes the
//! table's, spelled as the config spells them.

use http::Uri;
use kj::http::HeaderId;
use kj::http::HeaderTable;
use kj::http::HeadersRef;
use kj_rs::KjOwn;
use workerd_capnp::http_options;

use crate::Result;
use crate::bridge::ffi;
use crate::config::capnp_error;
use crate::config::optional_text;
use crate::config::text;

/// How URLs travel: whole, as a proxy is asked for them, or as a path with the host in the
/// `Host` header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Style {
    Host,
    Proxy,
}

pub struct HttpRewriter {
    style: Style,
    forwarded_proto_header: Option<String>,
    cf_blob_header: Option<String>,
    capnp_connect_host: Option<String>,
    request_headers: Vec<ffi::HeaderEdit>,
    response_headers: Vec<ffi::HeaderEdit>,
}

/// A request as the options leave it.
pub struct Rewritten {
    pub url: String,
    /// The headers to use in place of the request's, when the options change any.
    pub headers: Option<KjOwn<ffi::HttpHeaders>>,
}

fn injected(
    headers: capnp::struct_list::Reader<'_, http_options::header::Owned>,
) -> Result<Vec<ffi::HeaderEdit>> {
    headers
        .iter()
        .map(|header| {
            Ok(ffi::HeaderEdit {
                name: text(header.get_name())?,
                value: optional_text(header.has_value(), header.get_value())?.into(),
            })
        })
        .collect()
}

fn edit(name: &str, value: Option<&str>) -> ffi::HeaderEdit {
    ffi::HeaderEdit {
        name: name.to_owned(),
        value: value.map(str::to_owned).into(),
    }
}

impl HttpRewriter {
    pub fn new(options: http_options::Reader<'_>) -> Result<Self> {
        Ok(Self {
            style: match options.get_style() {
                Ok(http_options::Style::Host) => Style::Host,
                Ok(http_options::Style::Proxy) => Style::Proxy,
                Err(capnp::NotInSchema(_)) => {
                    return Err(kj::failed!(
                        "Encountered unknown HttpOptions::style setting. Was the config compiled \
                         with a newer version of the schema?"
                    ));
                }
            },
            forwarded_proto_header: optional_text(
                options.has_forwarded_proto_header(),
                options.get_forwarded_proto_header(),
            )?,
            cf_blob_header: optional_text(
                options.has_cf_blob_header(),
                options.get_cf_blob_header(),
            )?,
            capnp_connect_host: optional_text(
                options.has_capnp_connect_host(),
                options.get_capnp_connect_host(),
            )?,
            request_headers: injected(options.get_inject_request_headers().map_err(capnp_error)?)?,
            response_headers: injected(
                options.get_inject_response_headers().map_err(capnp_error)?,
            )?,
        })
    }

    #[must_use]
    pub const fn style(&self) -> Style {
        self.style
    }

    #[must_use]
    pub const fn has_cf_blob_header(&self) -> bool {
        self.cf_blob_header.is_some()
    }

    #[must_use]
    pub fn capnp_connect_host(&self) -> Option<&str> {
        self.capnp_connect_host.as_deref()
    }

    /// The edits `injectResponseHeaders` makes to a response's headers.
    #[must_use]
    pub fn response_edits(&self) -> &[ffi::HeaderEdit] {
        &self.response_headers
    }

    /// `headers` with `edits` and then the injected request headers applied; none when that
    /// changes nothing.
    fn edited(
        &self,
        table: &HeaderTable,
        headers: HeadersRef<'_>,
        edits: &[ffi::HeaderEdit],
    ) -> Result<Option<KjOwn<ffi::HttpHeaders>>> {
        let injected = &self.request_headers;
        if edits.is_empty() && injected.is_empty() {
            return Ok(None);
        }
        let edited = ffi::edit_headers(table, headers.as_ffi(), edits, injected)?;
        Ok(Some(edited))
    }

    /// Rewrites a request leaving for an external server. `url` is what the worker asked for: a
    /// proxy-form URL.
    pub fn rewrite_outgoing_request(
        &self,
        table: &HeaderTable,
        url: &str,
        headers: HeadersRef<'_>,
        cf_blob_json: Option<&str>,
    ) -> Result<Rewritten> {
        let mut url = url.to_owned();
        let mut edits = Vec::new();
        if self.style == Style::Host {
            let parsed: Uri = url
                .parse()
                .map_err(|_| kj::failed!("invalid outgoing request URL: {url}"))?;
            let authority = parsed
                .authority()
                .ok_or_else(|| kj::failed!("outgoing request URL has no host: {url}"))?
                .as_str();
            // The host and port, as `kj::Url`'s host: credentials stay out of the header.
            let host = authority
                .rsplit_once('@')
                .map_or(authority, |(_, host)| host);
            edits.push(edit("Host", Some(host)));
            if let Some(forwarded_proto) = &self.forwarded_proto_header {
                let scheme = parsed.scheme_str().unwrap_or("http");
                edits.push(edit(forwarded_proto, Some(scheme)));
            }
            parsed
                .path_and_query()
                .map_or("/", http::uri::PathAndQuery::as_str)
                .clone_into(&mut url);
        }
        if let Some(cf_blob) = &self.cf_blob_header {
            edits.push(edit(cf_blob, cf_blob_json));
        }
        let headers = self.edited(table, headers, &edits)?;
        Ok(Rewritten { url, headers })
    }

    /// Rewrites a request arriving on a socket. `url` is what the client sent; `physical_protocol`
    /// is the socket's ("http" or "https"). Returns the request to give the worker and the cf
    /// blob it carried, if the options name a header for it; none for a request the options can
    /// make no sense of, to be answered with 400.
    pub fn rewrite_incoming_request(
        &self,
        table: &HeaderTable,
        url: &str,
        physical_protocol: &str,
        headers: HeadersRef<'_>,
    ) -> Result<Option<(Rewritten, Option<String>)>> {
        let mut url = url.to_owned();
        let mut edits = Vec::new();
        if self.style == Style::Host {
            let Ok(parsed) = url.parse::<Uri>() else {
                return Ok(None);
            };
            let Some(host) = headers
                .get(HeaderId::HOST)
                .and_then(|host| std::str::from_utf8(host).ok())
            else {
                return Ok(None);
            };
            let mut scheme = physical_protocol;
            if let Some(header) = &self.forwarded_proto_header
                && let Some(forwarded) = headers.get_by_name(header)
            {
                let Ok(forwarded) = std::str::from_utf8(forwarded) else {
                    return Ok(None);
                };
                scheme = forwarded;
                edits.push(edit(header, None));
            }
            let path = parsed
                .path_and_query()
                .map_or("/", http::uri::PathAndQuery::as_str);
            url = format!("{scheme}://{host}{path}");
        }

        let mut cf_blob_json = None;
        if let Some(header) = &self.cf_blob_header
            && let Some(blob) = headers.get_by_name(header)
        {
            let Ok(blob) = std::str::from_utf8(blob) else {
                return Ok(None);
            };
            cf_blob_json = Some(blob.to_owned());
            edits.push(edit(header, None));
        }

        let headers = self.edited(table, headers, &edits)?;
        Ok(Some((Rewritten { url, headers }, cf_blob_json)))
    }
}

#[cfg(test)]
#[path = "rewriter-test.rs"]
mod tests;
