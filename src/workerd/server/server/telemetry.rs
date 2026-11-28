// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The OpenTelemetry environment variables (`TracingOptions` in workerd.capnp).
//!
//! `WORKERD_OTEL=1` is the command line's (cli/config.rs), which adds the telemetry Worker to the
//! config. A collector that `OTEL_EXPORTER_OTLP_ENDPOINT` names is made here, as an external
//! server built in memory, so that the endpoint's path can prefix `/v1/traces` and `/v1/logs`.

use std::rc::Rc;

use percent_encoding::percent_decode_str;
use url::Url;
use workerd_capnp::external_server;

use crate::Result;
use crate::channels::Channel;
use crate::config::Factory;
use crate::services::make_external_service;

/// `OTEL_EXPORTER_OTLP_ENDPOINT` and its companions.
pub struct Collector {
    pub url: Url,
    pub headers: Vec<(String, String)>,
    /// `OTEL_EXPORTER_OTLP_PROTOCOL=http/json`.
    pub json: bool,
    pub sample_ratio: f64,
}

impl Collector {
    /// `None` when `OTEL_EXPORTER_OTLP_ENDPOINT` is unset; `Err` says which variable is wrong.
    pub fn read() -> std::result::Result<Option<Self>, String> {
        Self::read_from(|name| std::env::var(name).ok())
    }

    fn read_from(
        get: impl Fn(&str) -> Option<String>,
    ) -> std::result::Result<Option<Self>, String> {
        let var = |name: &str| get(name).filter(|v| !v.is_empty());
        let Some(text) = var(ENDPOINT) else {
            return Ok(None);
        };
        let bad =
            || format!("{ENDPOINT}={text}: expected an http(s) URL with no query or fragment");
        let url = Url::parse(&text).map_err(|_| bad())?;
        if !matches!(url.scheme(), "http" | "https")
            || url.host_str().is_none()
            || url.query().is_some()
            || url.fragment().is_some()
        {
            return Err(bad());
        }
        let headers =
            var("OTEL_EXPORTER_OTLP_HEADERS").map_or(Ok(Vec::new()), |list| headers(&list))?;
        let json = match var("OTEL_EXPORTER_OTLP_PROTOCOL").as_deref() {
            None | Some("http/protobuf") => false,
            Some("http/json") => true,
            Some(p) => {
                return Err(format!(
                    "OTEL_EXPORTER_OTLP_PROTOCOL={p}: expected http/protobuf or http/json"
                ));
            }
        };
        let sample_ratio = sample_ratio(&var)?;
        Ok(Some(Self {
            url,
            headers,
            json,
            sample_ratio,
        }))
    }
}

/// Sends traces and logs to the bundled telemetry Worker; read by the command line.
pub const WORKERD_OTEL: &str = "WORKERD_OTEL";
/// Names a collector.
pub const ENDPOINT: &str = "OTEL_EXPORTER_OTLP_ENDPOINT";

/// `OTEL_TRACES_SAMPLER_ARG`, a ratio from 0 to 1, or 1 when unset.
pub fn sample_ratio(var: &dyn Fn(&str) -> Option<String>) -> std::result::Result<f64, String> {
    const NAME: &str = "OTEL_TRACES_SAMPLER_ARG";
    var(NAME).map_or(Ok(1.0), |v| {
        v.parse()
            .ok()
            .filter(|r| (0.0..=1.0).contains(r))
            .ok_or_else(|| format!("{NAME}={v}: expected a ratio from 0 to 1"))
    })
}

/// `OTEL_EXPORTER_OTLP_HEADERS`: comma-separated `name=value`, values percent-encoded.
fn headers(list: &str) -> std::result::Result<Vec<(String, String)>, String> {
    list.split(',')
        .filter(|h| !h.trim().is_empty())
        .map(|h| {
            let (name, value) = h
                .split_once('=')
                .ok_or_else(|| format!("OTEL_EXPORTER_OTLP_HEADERS: {h}: expected name=value"))?;
            let value = percent_decode_str(value.trim()).decode_utf8_lossy();
            Ok((name.trim().to_owned(), value.into_owned()))
        })
        .collect()
}

/// The collector at `url` as an external HTTP(S) service sending `headers`, and the path its
/// `/v1/traces` and `/v1/logs` sit under.
pub fn collector(
    factory: &Rc<Factory>,
    url: &Url,
    headers: &[(String, String)],
) -> Result<(Rc<dyn Channel>, String)> {
    let mut message = capnp::message::Builder::new_default();
    let mut conf = message.init_root::<external_server::Builder<'_>>();
    let host = url.host_str().unwrap_or_default();
    let port = url.port_or_known_default().unwrap_or(80);
    conf.set_address(format!("{host}:{port}"));
    let options = if url.scheme() == "https" {
        let mut https = conf.init_https();
        https
            .reborrow()
            .init_tls_options()
            .set_trust_browser_cas(true);
        https.init_options()
    } else {
        conf.init_http()
    };
    let len = u32::try_from(headers.len()).unwrap_or(u32::MAX);
    let mut inject = options.init_inject_request_headers(len);
    for (i, (name, value)) in (0..len).zip(headers) {
        let mut header = inject.reborrow().get(i);
        header.set_name(name);
        header.set_value(value);
    }
    let reader = message
        .get_root_as_reader::<external_server::Reader<'_>>()
        .map_err(|e| kj::failed!("{e}"))?;
    let service = make_external_service(ENDPOINT, reader, None, factory)?;
    Ok((service, url.path().trim_end_matches('/').to_owned()))
}

#[cfg(test)]
#[path = "telemetry-test.rs"]
mod tests;
