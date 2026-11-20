// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Telemetry in the cluster bucket (`TracingOptions.bucket`, or `WORKERD_OTEL=1`): the spans and
//! logs the OTLP exporter encodes, written as Parquet files that `DuckDB` queries in place.
//!
//! C++ hands over each encoded request as it would POST it (`telemetry_spans`,
//! `telemetry_logs`). The rows buffer here until `flush` passes or they reach about
//! `flush_bytes`, then go out as one file per signal under
//! `<prefix>telemetry/{traces,logs}/<node>/<yyyy>/<mm>/<dd>/<hh>/<unix_us>-<rand>.parquet`.
//! A failed upload drops its file: telemetry never holds up requests.

use std::cell::RefCell;
use std::rc::Rc;
use std::time::Duration;

use cluster::Bucket;
use cluster::now_ms;
use cluster::s3::format_rfc3339_nano;
use otlp::proto::ExportLogsServiceRequest;
use otlp::proto::ExportTraceServiceRequest;
use otlp_parquet::Log;
use otlp_parquet::Span;
use percent_encoding::percent_decode_str;
use prost::Message;
use url::Url;
use workerd_capnp::external_server;

use crate::Result;
use crate::channels::Channel;
use crate::config::Factory;
use crate::services::make_external_service;

/// `WORKERD_OTEL` and its companions (workerd.capnp's `TracingOptions`).
pub enum Env {
    /// `WORKERD_OTEL=1`: Parquet in the cluster bucket.
    Bucket {
        flush: Duration,
        flush_bytes: usize,
        /// `None` keeps files until the bucket's own lifecycle rules delete them.
        retention: Option<Duration>,
    },
    /// `WORKERD_OTEL=<url>`: OTLP/HTTP to the collector at `url`, traces and logs.
    Collector {
        url: Url,
        headers: Vec<(String, String)>,
    },
}

impl Env {
    /// `None` when `WORKERD_OTEL` is unset or `0`; `Err` says which variable is wrong.
    pub fn read() -> std::result::Result<Option<Self>, String> {
        Self::read_from(|name| std::env::var(name).ok())
    }

    fn read_from(
        get: impl Fn(&str) -> Option<String>,
    ) -> std::result::Result<Option<Self>, String> {
        let var = |name: &str| get(name).filter(|v| !v.is_empty());
        // Positive: a zero flush interval would spin.
        let number = |name: &str, default: u64| match var(name) {
            None => Ok(default),
            Some(v) => v
                .parse()
                .ok()
                .filter(|n| *n > 0)
                .ok_or_else(|| format!("{name}={v}: expected a positive number")),
        };
        match var("WORKERD_OTEL").as_deref() {
            None | Some("0") => Ok(None),
            Some("1") => Ok(Some(Self::Bucket {
                flush: Duration::from_millis(number("WORKERD_OTEL_FLUSH_MS", 300_000)?),
                flush_bytes: usize::try_from(number("WORKERD_OTEL_FLUSH_BYTES", 5 << 20)?)
                    .unwrap_or(usize::MAX),
                retention: retention(var("WORKERD_OTEL_RETENTION").as_deref().unwrap_or("30d"))?,
            })),
            Some(text) => {
                let bad = || {
                    format!(
                        "WORKERD_OTEL={text}: expected 0, 1, or an http(s) collector URL with no \
                         query or fragment"
                    )
                };
                let url = Url::parse(text).map_err(|_| bad())?;
                if !matches!(url.scheme(), "http" | "https")
                    || url.host_str().is_none()
                    || url.query().is_some()
                    || url.fragment().is_some()
                {
                    return Err(bad());
                }
                let headers = var("OTEL_EXPORTER_OTLP_HEADERS")
                    .map_or(Ok(Vec::new()), |list| headers(&list))?;
                Ok(Some(Self::Collector { url, headers }))
            }
        }
    }
}

/// `<days>d`, or `none`.
fn retention(text: &str) -> std::result::Result<Option<Duration>, String> {
    if text == "none" {
        return Ok(None);
    }
    let days = text.strip_suffix('d').and_then(|d| d.parse::<u64>().ok());
    let days =
        days.ok_or_else(|| format!("WORKERD_OTEL_RETENTION={text}: expected <days>d or none"))?;
    Ok(Some(Duration::from_secs(days * 86_400)))
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
    let service = make_external_service("WORKERD_OTEL", reader, None, factory)?;
    Ok((service, url.path().trim_end_matches('/').to_owned()))
}

#[derive(Default)]
struct Buffer {
    spans: Vec<Span>,
    logs: Vec<Log>,
    bytes: usize,
}

struct Inner {
    bucket: Bucket,
    node: String,
    flush_bytes: usize,
    buffer: RefCell<Buffer>,
}

/// The sink C++ holds, as a `Box`; clones share one buffer.
#[derive(Clone)]
pub struct TelemetrySink {
    inner: Rc<Inner>,
    factory: Rc<Factory>,
}

impl TelemetrySink {
    /// A sink writing below `bucket`, which flushes every `flush` until the run ends, and with
    /// a `retention` deletes older files at the start and every six hours after.
    pub fn new(
        factory: &Rc<Factory>,
        bucket: Bucket,
        node: &str,
        flush: Duration,
        flush_bytes: usize,
        retention: Option<Duration>,
    ) -> Self {
        let sink = Self {
            inner: Rc::new(Inner {
                bucket,
                node: node.to_owned(),
                flush_bytes,
                buffer: RefCell::default(),
            }),
            factory: Rc::clone(factory),
        };
        let weak = Rc::downgrade(&sink.inner);
        let factory_ = Rc::clone(factory);
        factory.spawn_detached(async move {
            loop {
                factory_.sleep(flush).await;
                let Some(inner) = weak.upgrade() else { return };
                flush_now(&inner).await;
            }
        });
        if let Some(retention) = retention {
            let weak = Rc::downgrade(&sink.inner);
            let factory_ = Rc::clone(factory);
            factory.spawn_detached(async move {
                loop {
                    let Some(inner) = weak.upgrade() else { return };
                    sweep(&inner.bucket, retention).await;
                    drop(inner);
                    factory_.sleep(SWEEP_INTERVAL).await;
                }
            });
        }
        sink
    }

    fn add(&self, spans: Vec<Span>, logs: Vec<Log>) {
        let full = {
            let mut buffer = self.inner.buffer.borrow_mut();
            buffer.bytes += spans.iter().map(Span::size).sum::<usize>();
            buffer.bytes += logs.iter().map(Log::size).sum::<usize>();
            buffer.spans.extend(spans);
            buffer.logs.extend(logs);
            buffer.bytes >= self.inner.flush_bytes
        };
        if full {
            let inner = Rc::clone(&self.inner);
            self.factory
                .spawn_detached(async move { flush_now(&inner).await });
        }
    }
}

/// Encodes on tokio's blocking pool, so requests don't wait on it.
async fn flush_now(inner: &Inner) {
    let Buffer { spans, logs, .. } = inner.buffer.take();
    let now = now_ms();
    let node = inner.node.clone();
    let files = tokio::task::spawn_blocking(move || {
        let traces = (!spans.is_empty()).then(|| otlp_parquet::encode_spans(&spans, &node));
        let logs = (!logs.is_empty()).then(|| otlp_parquet::encode_logs(&logs, &node));
        [("traces", traces), ("logs", logs)]
    })
    .await;
    for (signal, file) in files.into_iter().flatten() {
        if let Some(file) = file {
            put(inner, signal, now, file.map_err(|e| e.to_string())).await;
        }
    }
}

async fn put(inner: &Inner, signal: &str, now_ms: u64, file: std::result::Result<Vec<u8>, String>) {
    let key = key(signal, &inner.node, now_ms, rand::random());
    let metadata = [("workerd-schema", otlp_parquet::SCHEMA.to_owned())];
    let written = match file {
        Ok(file) => inner
            .bucket
            .put(&key, file, &metadata)
            .await
            .map_err(|e| e.to_string()),
        Err(e) => Err(e),
    };
    if let Err(e) = written {
        tracing::warn!("telemetry: writing {key}: {e}");
    }
}

const SWEEP_INTERVAL: Duration = Duration::from_hours(6);

/// Deletes every node's files from hours that began before `retention` ago. A failure leaves
/// the rest for the next sweep.
async fn sweep(bucket: &Bucket, retention: Duration) {
    let retention_ms = u64::try_from(retention.as_millis()).unwrap_or(u64::MAX);
    // `yyyy/mm/dd/hh` of the oldest hour kept; keys compare as text.
    let oldest = hour(now_ms().saturating_sub(retention_ms));
    for signal in ["traces/", "logs/"] {
        let Ok(nodes) = bucket.prefixes(signal).await else {
            continue;
        };
        for node in nodes {
            let Ok(files) = bucket.list(&node).await else {
                continue;
            };
            for file in files {
                let old = file
                    .key
                    .get(node.len()..node.len() + 13)
                    .is_some_and(|h| *h < *oldest);
                if !old {
                    continue;
                }
                if let Err(e) = bucket.delete(&file.key).await {
                    tracing::warn!("telemetry: deleting {}: {e}", file.key);
                }
            }
        }
    }
}

/// `yyyy/mm/dd/hh` in UTC.
fn hour(unix_ms: u64) -> String {
    let time = format_rfc3339_nano(i64::try_from(unix_ms).unwrap_or(i64::MAX));
    format!(
        "{}/{}/{}/{}",
        &time[0..4],
        &time[5..7],
        &time[8..10],
        &time[11..13]
    )
}

/// `traces/<node>/<yyyy>/<mm>/<dd>/<hh>/<unix_us>-<tag>.parquet`.
fn key(signal: &str, node: &str, now_ms: u64, tag: u32) -> String {
    let us = now_ms * 1000;
    format!("{signal}/{node}/{}/{us}-{tag:08x}.parquet", hour(now_ms))
}

pub fn telemetry_spans(sink: &TelemetrySink, request: &[u8]) {
    let Ok(request) = ExportTraceServiceRequest::decode(request) else {
        tracing::warn!("telemetry: dropping spans that do not decode");
        return;
    };
    sink.add(otlp_parquet::spans(request), Vec::new());
}

pub fn telemetry_logs(sink: &TelemetrySink, request: &[u8]) {
    let Ok(request) = ExportLogsServiceRequest::decode(request) else {
        tracing::warn!("telemetry: dropping logs that do not decode");
        return;
    };
    sink.add(Vec::new(), otlp_parquet::logs(request));
}

#[cfg(test)]
#[path = "telemetry-test.rs"]
mod tests;
