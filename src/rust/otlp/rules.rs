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

/// Whether a `fetch` span targets the placeholder host of a binding that is implemented with an
/// internal fetch (AI, D1, Workflows, Images, Vectorize). Those are an implementation detail of
/// the binding and are not reported. Reads `server.address`, so `add_url_attributes` goes first.
pub fn is_internal_binding_fetch(name: &str, attributes: &Attributes) -> bool {
    const INTERNAL_HOSTS: [&str; 6] = [
        "workers-binding.ai",
        "d1",
        "workflow-binding.local",
        "fake.host",
        "js.images.cloudflare.com",
        "vector-search",
    ];
    name == "fetch"
        && attributes
            .string("server.address")
            .is_some_and(|address| INTERNAL_HOSTS.contains(&address))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proto::any_value::Value;

    fn with(pairs: &[(&str, Value)]) -> Attributes {
        let mut attributes = Attributes::default();
        for (key, value) in pairs {
            attributes.push(*key, value.clone());
        }
        attributes
    }

    fn string(value: &str) -> Value {
        Value::String(value.to_owned())
    }

    #[test]
    fn kind_follows_the_trigger_for_the_root_and_the_name_for_children() {
        let timer = with(&[("faas.trigger", string("timer"))]);
        let http = with(&[("faas.trigger", string("http"))]);
        assert_eq!(span_kind("scheduled", &timer, true), SpanKind::Consumer);
        assert_eq!(span_kind("GET", &http, true), SpanKind::Server);
        assert_eq!(
            span_kind("GET", &Attributes::default(), true),
            SpanKind::Server
        );
        // The trigger only matters on the root.
        assert_eq!(span_kind("fetch", &timer, false), SpanKind::Client);
        assert_eq!(span_kind("queue_send", &http, false), SpanKind::Producer);
        assert_eq!(
            span_kind("durable_object_storage_setAlarm", &http, false),
            SpanKind::Producer
        );
    }

    #[test]
    fn error_type_is_derived_from_outcome_and_status_code() {
        let status = |code| with(&[("http.response.status_code", Value::Int(code))]);
        assert_eq!(
            derived_error_type(&status(404), false).as_deref(),
            Some("404")
        );
        assert_eq!(derived_error_type(&status(399), false), None);
        // A 4xx from the worker itself is the worker's answer, not its failure.
        assert_eq!(derived_error_type(&status(404), true), None);
        assert_eq!(
            derived_error_type(&status(503), true).as_deref(),
            Some("503")
        );

        let outcome = |outcome| {
            with(&[
                ("cloudflare.outcome", string(outcome)),
                ("http.response.status_code", Value::Int(500)),
            ])
        };
        assert_eq!(
            derived_error_type(&outcome("exceededCpu"), true).as_deref(),
            Some("exceededCpu")
        );
        assert_eq!(
            derived_error_type(&outcome("ok"), true).as_deref(),
            Some("500")
        );
        // The outcome belongs to the root.
        assert_eq!(
            derived_error_type(&with(&[("cloudflare.outcome", string("exception"))]), false),
            None
        );
    }

    #[test]
    fn url_attributes_are_split_from_url_full() {
        let mut attributes = with(&[(
            "url.full",
            string("https://example.com:8443/a/b?x=1&y#frag"),
        )]);
        add_url_attributes(&mut attributes, false);
        assert_eq!(attributes.string("url.scheme"), Some("https"));
        assert_eq!(attributes.string("server.address"), Some("example.com"));
        assert_eq!(attributes.int("server.port"), Some(8443));
        assert_eq!(attributes.string("url.path"), Some("/a/b"));
        assert_eq!(attributes.string("url.query"), Some("x=1&y"));
        assert_eq!(attributes.string("network.protocol.name"), Some("http"));
        assert_eq!(
            attributes.string("url.full"),
            Some("https://example.com:8443/a/b?x=1&y#frag")
        );
    }

    #[test]
    fn url_without_port_path_or_query() {
        let mut attributes = with(&[("url.full", string("http://[::1]"))]);
        add_url_attributes(&mut attributes, false);
        assert_eq!(attributes.string("server.address"), Some("[::1]"));
        assert_eq!(attributes.get("server.port"), None);
        assert_eq!(attributes.string("url.path"), Some("/"));
        assert_eq!(attributes.get("url.query"), None);
    }

    #[test]
    fn redaction_cuts_the_query_from_url_full() {
        let mut attributes = with(&[("url.full", string("https://example.com:8443/p?secret=1#f"))]);
        add_url_attributes(&mut attributes, true);
        assert_eq!(
            attributes.string("url.full"),
            Some("https://example.com:8443/p")
        );
        assert_eq!(attributes.string("url.path"), Some("/p"));
        assert_eq!(attributes.get("url.query"), None);

        // Nothing to redact: url.full is not rewritten.
        let mut attributes = with(&[("url.full", string("https://example.com/p#f"))]);
        add_url_attributes(&mut attributes, true);
        assert_eq!(
            attributes.string("url.full"),
            Some("https://example.com/p#f")
        );
    }

    #[test]
    fn url_attributes_keep_what_the_span_already_carries() {
        let mut attributes = with(&[
            ("server.address", string("origin.internal")),
            ("url.full", string("https://example.com/p?q=1")),
        ]);
        add_url_attributes(&mut attributes, false);
        assert_eq!(attributes.string("server.address"), Some("origin.internal"));
        assert_eq!(attributes.string("url.query"), Some("q=1"));

        // Neither an absent nor an unparseable url.full adds anything.
        let mut attributes = with(&[("url.full", string("not a url"))]);
        add_url_attributes(&mut attributes, false);
        assert_eq!(attributes, with(&[("url.full", string("not a url"))]));
        let mut attributes = with(&[("url.full", Value::Int(1))]);
        add_url_attributes(&mut attributes, false);
        assert_eq!(attributes.get("url.scheme"), None);
    }

    #[test]
    fn internal_binding_fetches_are_recognised_by_host() {
        let to = |address| with(&[("server.address", string(address))]);
        assert!(is_internal_binding_fetch("fetch", &to("d1")));
        assert!(!is_internal_binding_fetch("fetch", &to("example.com")));
        assert!(!is_internal_binding_fetch("d1_query", &to("d1")));
        assert!(!is_internal_binding_fetch("fetch", &Attributes::default()));
    }
}
