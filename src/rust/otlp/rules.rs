// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! What a span's name and attributes imply for its OTLP form: its kind, an error status the
//! worker did not set itself, the URL attributes, and whether it is reported at all.

use ada_url::Url;

use crate::attributes::Attributes;
use crate::proto::SpanKind;

/// The root span is `Server`, or `Consumer` when the invocation was triggered by a timer, a queue
/// or an email. Every other span describes an outgoing operation: `Producer` for the ones that
/// enqueue work, `Client` otherwise.
pub fn span_kind(name: &str, attributes: &Attributes, is_root: bool) -> SpanKind {
    if is_root {
        match attributes.string("faas.trigger") {
            Some("timer" | "pubsub" | "email") => SpanKind::Consumer,
            _ => SpanKind::Server,
        }
    } else if matches!(name, "queue_send" | "durable_object_storage_setAlarm") {
        SpanKind::Producer
    } else {
        SpanKind::Client
    }
}

/// The `error.type` of a span whose attributes alone make it an error: a root span whose
/// `cloudflare.outcome` is not `ok` or whose response status is 5xx, and any other span whose
/// response status is 4xx or 5xx.
pub fn derived_error_type(attributes: &Attributes, is_root: bool) -> Option<String> {
    let status_code = attributes.int("http.response.status_code").unwrap_or(0);
    if is_root {
        if let Some(outcome) = attributes
            .string("cloudflare.outcome")
            .filter(|o| *o != "ok")
        {
            return Some(outcome.to_owned());
        }
        (status_code >= 500).then(|| status_code.to_string())
    } else {
        (status_code >= 400).then(|| status_code.to_string())
    }
}

/// Adds the attributes derived from `url.full` (`url.scheme`, `server.address`, `server.port` when
/// the URL names one, `url.path`, `url.query` when there is a query, and `network.protocol.name`
/// for HTTP), keeping any the span already carries. With `redact_query_string` the query and
/// fragment are cut from `url.full` instead and `url.query` is not set. A span without a
/// parseable `url.full` is left alone.
pub fn add_url_attributes(attributes: &mut Attributes, redact_query_string: bool) {
    let Some(url) = attributes
        .string("url.full")
        .and_then(|full| Url::parse(full, None).ok())
    else {
        return;
    };
    let scheme = url.protocol().trim_end_matches(':');

    attributes.set_default("url.scheme", scheme);
    attributes.set_default("server.address", url.hostname());
    if let Ok(port) = url.port().parse::<u16>() {
        attributes.set_default("server.port", i64::from(port));
    }
    attributes.set_default("url.path", url.pathname());
    if matches!(scheme, "http" | "https") {
        attributes.set_default("network.protocol.name", "http");
    }

    let query = url.search().trim_start_matches('?');
    if query.is_empty() {
        return;
    }
    if redact_query_string {
        let redacted = format!("{scheme}://{}{}", url.host(), url.pathname());
        attributes.set("url.full", redacted);
    } else {
        attributes.set_default("url.query", query);
    }
}

#[cfg(test)]
#[path = "rules-test.rs"]
mod tests;
