// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::collections::HashMap;

use super::Collector;

fn env(vars: &[(&str, &str)]) -> Result<Option<Collector>, String> {
    let vars: HashMap<String, String> = vars
        .iter()
        .map(|(k, v)| ((*k).to_owned(), (*v).to_owned()))
        .collect();
    Collector::read_from(|name| vars.get(name).cloned())
}

#[test]
fn endpoint_reads_values_and_defaults() {
    assert!(env(&[]).unwrap().is_none());
    let c = env(&[("OTEL_EXPORTER_OTLP_ENDPOINT", "http://c:4318")])
        .unwrap()
        .unwrap();
    assert!(!c.json);
    assert!((c.sample_ratio - 1.0).abs() < f64::EPSILON);
    let c = env(&[
        (
            "OTEL_EXPORTER_OTLP_ENDPOINT",
            "https://otel.example.com:4318/base/",
        ),
        (
            "OTEL_EXPORTER_OTLP_HEADERS",
            "authorization=Bearer%20t, x-a = b",
        ),
        ("OTEL_EXPORTER_OTLP_PROTOCOL", "http/json"),
        ("OTEL_TRACES_SAMPLER_ARG", "0.25"),
    ])
    .unwrap()
    .unwrap();
    assert_eq!(c.url.as_str(), "https://otel.example.com:4318/base/");
    assert_eq!(
        c.headers,
        [
            ("authorization".to_owned(), "Bearer t".to_owned()),
            ("x-a".to_owned(), "b".to_owned())
        ]
    );
    assert!(c.json);
    assert!((c.sample_ratio - 0.25).abs() < f64::EPSILON);
}

#[test]
fn errors_name_the_variable() {
    let error = |vars: &[(&str, &str)]| env(vars).err().unwrap();
    let endpoint = |url| [("OTEL_EXPORTER_OTLP_ENDPOINT", url)];
    assert!(error(&endpoint("http://c/?q=1")).starts_with("OTEL_EXPORTER_OTLP_ENDPOINT="));
    assert!(error(&endpoint("ftp://c")).starts_with("OTEL_EXPORTER_OTLP_ENDPOINT="));
    let with = |name, value| [("OTEL_EXPORTER_OTLP_ENDPOINT", "http://c"), (name, value)];
    assert!(
        error(&with("OTEL_EXPORTER_OTLP_PROTOCOL", "grpc"))
            .starts_with("OTEL_EXPORTER_OTLP_PROTOCOL=grpc")
    );
    assert!(error(&with("OTEL_TRACES_SAMPLER_ARG", "2")).starts_with("OTEL_TRACES_SAMPLER_ARG=2"));
}
