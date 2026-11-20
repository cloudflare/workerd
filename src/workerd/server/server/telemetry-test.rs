// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::collections::HashMap;
use std::time::Duration;

use super::Env;
use super::expired;
use super::hour;
use super::key;

#[test]
fn key_partitions_by_node_and_hour() {
    // 2026-11-14T08:58:20.123Z
    let key = key("traces", "n1", 1_794_646_700_123, 0xbeef);
    assert_eq!(
        key,
        "traces/n1/2026/11/14/08/1794646700123000-0000beef.parquet"
    );
}

fn env(vars: &[(&str, &str)]) -> Result<Option<Env>, String> {
    let vars: HashMap<String, String> = vars
        .iter()
        .map(|(k, v)| ((*k).to_owned(), (*v).to_owned()))
        .collect();
    Env::read_from(|name| vars.get(name).cloned())
}

#[test]
fn workerd_otel_reads_values_and_defaults() {
    assert!(env(&[]).unwrap().is_none());
    assert!(env(&[("WORKERD_OTEL", "0")]).unwrap().is_none());
    let Some(Env::Bucket {
        flush,
        flush_bytes,
        retention,
    }) = env(&[("WORKERD_OTEL", "1")]).unwrap()
    else {
        panic!()
    };
    assert_eq!(flush, Duration::from_mins(5));
    assert_eq!(flush_bytes, 5 << 20);
    assert_eq!(retention, Some(Duration::from_hours(30 * 24)));
    let Some(Env::Bucket {
        flush, retention, ..
    }) = env(&[
        ("WORKERD_OTEL", "1"),
        ("WORKERD_OTEL_FLUSH_MS", "5000"),
        ("WORKERD_OTEL_RETENTION", "none"),
    ])
    .unwrap()
    else {
        panic!()
    };
    assert_eq!(flush, Duration::from_secs(5));
    assert_eq!(retention, None);
    let Some(Env::Collector { url, headers }) = env(&[
        ("WORKERD_OTEL", "https://otel.example.com:4318/base/"),
        (
            "OTEL_EXPORTER_OTLP_HEADERS",
            "authorization=Bearer%20t, x-a = b",
        ),
    ])
    .unwrap() else {
        panic!()
    };
    assert_eq!(url.as_str(), "https://otel.example.com:4318/base/");
    assert_eq!(
        headers,
        [
            ("authorization".to_owned(), "Bearer t".to_owned()),
            ("x-a".to_owned(), "b".to_owned())
        ]
    );
}

#[test]
fn workerd_otel_errors_name_the_variable() {
    let error = |vars| env(vars).err().unwrap();
    assert!(error(&[("WORKERD_OTEL", "yes")]).starts_with("WORKERD_OTEL=yes"));
    assert!(error(&[("WORKERD_OTEL", "http://c/?q=1")]).starts_with("WORKERD_OTEL="));
    assert!(error(&[("WORKERD_OTEL", "ftp://c")]).starts_with("WORKERD_OTEL="));
    let retention = [("WORKERD_OTEL", "1"), ("WORKERD_OTEL_RETENTION", "30")];
    assert!(error(&retention).starts_with("WORKERD_OTEL_RETENTION=30"));
    let flush = [("WORKERD_OTEL", "1"), ("WORKERD_OTEL_FLUSH_MS", "soon")];
    assert!(error(&flush).starts_with("WORKERD_OTEL_FLUSH_MS=soon"));
    let zero = [("WORKERD_OTEL", "1"), ("WORKERD_OTEL_FLUSH_BYTES", "0")];
    assert!(error(&zero).starts_with("WORKERD_OTEL_FLUSH_BYTES=0"));
}

#[test]
fn sweep_deletes_only_hours_before_the_oldest_kept() {
    let node = "traces/n1/";
    let oldest = "2026/11/14/08";
    assert!(expired("traces/n1/2026/11/14/07/1-0.parquet", node, oldest));
    assert!(!expired(
        "traces/n1/2026/11/14/08/1-0.parquet",
        node,
        oldest
    ));
    assert!(!expired(
        "traces/n1/2027/01/01/00/1-0.parquet",
        node,
        oldest
    ));
    assert!(!expired("traces/n1/x", node, oldest));
    assert!(!expired(
        "traces/n2/2026/11/14/07/1-0.parquet",
        node,
        oldest
    ));
}

#[test]
fn hours_compare_as_text() {
    // 2026-11-14T08:58:20Z and 2026-11-14T09:00:00Z
    assert_eq!(hour(1_794_646_700_123), "2026/11/14/08");
    assert!(*hour(1_794_646_700_123) < *hour(1_794_646_800_000));
}
