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
