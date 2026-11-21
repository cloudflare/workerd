// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn cidr(text: &str) -> Destination {
    Destination::Cidr(text.parse().unwrap())
}

fn hostname(text: &str) -> Destination {
    Destination::Hostname(text.to_owned())
}

#[test]
fn strip_port_forms() {
    assert_eq!(
        strip_port("10.0.0.1:8080").unwrap(),
        ("10.0.0.1".to_owned(), Some(8080))
    );
    assert_eq!(
        strip_port("10.0.0.1").unwrap(),
        ("10.0.0.1".to_owned(), None)
    );
    assert_eq!(
        strip_port("[::1]:8080").unwrap(),
        ("::1".to_owned(), Some(8080))
    );
    assert_eq!(strip_port("[::1]").unwrap(), ("::1".to_owned(), None));
    // IPv6 without brackets has no port.
    assert_eq!(strip_port("fe80::1").unwrap(), ("fe80::1".to_owned(), None));
    assert_eq!(strip_port("::1:80").unwrap(), ("::1:80".to_owned(), None));
    // A suffix that is not a port stays part of the host.
    assert_eq!(
        strip_port("example.com:http").unwrap(),
        ("example.com:http".to_owned(), None)
    );
}

#[test]
fn strip_port_errors() {
    let description = |text| strip_port(text).unwrap_err().description().to_owned();
    assert!(description("[::1").contains("Unclosed '[' in address string."));
    assert!(description("[::1]x").contains("Expected port suffix after ']'."));
    assert!(description("[::1]:").contains("Invalid port number."));
    assert!(description("[::1]:70000").contains("Invalid port number."));
}

#[test]
fn parse_host_port_forms() {
    assert_eq!(
        parse_host_port("10.0.0.1").unwrap(),
        (cidr("10.0.0.1/32"), None)
    );
    assert_eq!(
        parse_host_port("10.1.2.3/8:8080").unwrap(),
        (cidr("10.0.0.0/8"), Some(8080))
    );
    assert_eq!(
        parse_host_port("[fe80::1/10]:443").unwrap(),
        (cidr("fe80::/10"), Some(443))
    );
    assert_eq!(parse_host_port("::1").unwrap(), (cidr("::1/128"), None));
    assert_eq!(
        parse_host_port("Example.COM:7070").unwrap(),
        (hostname("example.com"), Some(7070))
    );
    assert_eq!(
        parse_host_port("*.example.com").unwrap(),
        (hostname("*.example.com"), None)
    );
    // Not an address, so a hostname glob.
    assert_eq!(
        parse_host_port("10.0.0.1/40").unwrap(),
        (hostname("10.0.0.1/40"), None)
    );
    assert!(
        parse_host_port(":80")
            .unwrap_err()
            .description()
            .contains("Host must not be empty.")
    );
}

#[test]
fn hostname_globs() {
    assert!(hostname_glob_matches("cloudflare.com", "cloudflare.com"));
    assert!(!hostname_glob_matches("cloudflare.com", "cloudflare.org"));
    assert!(hostname_glob_matches(
        "cloudflare.*.com",
        "cloudflare.dev.com"
    ));
    assert!(hostname_glob_matches(
        "cloudflare.*.com",
        "cloudflare.a.b.com"
    ));
    assert!(!hostname_glob_matches("cloudflare.*.com", "cloudflare.com"));
    assert!(hostname_glob_matches("*", "anything.example"));
    assert!(hostname_glob_matches("*.example.com", "a.example.com"));
    assert!(!hostname_glob_matches("*.example.com", "example.com"));
    assert!(hostname_glob_matches("a**b", "ab"));
    assert!(!hostname_glob_matches("", "a"));
}

#[test]
fn hostnames_normalize() {
    assert_eq!(normalize_hostname("Example.COM").unwrap(), "example.com");
    assert_eq!(
        normalize_hostname("example.com:8080").unwrap(),
        "example.com"
    );
    assert_eq!(
        normalize_hostname("bücher.example").unwrap(),
        "xn--bcher-kva.example"
    );
    assert!(
        normalize_hostname("exa mple.com")
            .unwrap_err()
            .description()
            .contains("Invalid X-Hostname URL hint.")
    );
}

fn mappings() -> Mappings<&'static str> {
    let mut mappings = Mappings::default();
    mappings.upsert(hostname("*.example.com"), 0, Protocol::Http, "glob");
    mappings.upsert(hostname("api.test"), 8080, Protocol::Http, "api-8080");
    mappings.upsert(cidr("10.0.0.0/8"), 0, Protocol::Http, "ten");
    mappings.upsert(cidr("10.0.0.0/8"), 5432, Protocol::Tcp, "postgres");
    mappings.upsert(hostname("secure.test"), 0, Protocol::Https, "tls");
    mappings
}

#[test]
fn find_by_hostname_and_port() {
    let mappings = mappings();
    let find =
        |dest, port, hostname, protocol| mappings.find(dest, port, hostname, protocol).unwrap();
    assert_eq!(
        find("1.2.3.4:80", 80, Some("www.example.com"), Protocol::Http),
        Some("glob")
    );
    // The hostname hint is normalized.
    assert_eq!(
        find("1.2.3.4:80", 80, Some("WWW.Example.com"), Protocol::Http),
        Some("glob")
    );
    assert_eq!(
        find("1.2.3.4:8080", 80, Some("api.test"), Protocol::Http),
        Some("api-8080")
    );
    assert_eq!(
        find("1.2.3.4:80", 80, Some("api.test"), Protocol::Http),
        None
    );
    // The default port applies to an address without one.
    assert_eq!(
        find("1.2.3.4", 8080, Some("api.test"), Protocol::Http),
        Some("api-8080")
    );
    // Another protocol's mapping does not match.
    assert_eq!(
        find("1.2.3.4:443", 443, Some("api.test"), Protocol::Https),
        None
    );
    assert_eq!(
        find("1.2.3.4:443", 443, Some("secure.test"), Protocol::Https),
        Some("tls")
    );
}

#[test]
fn find_by_address() {
    let mappings = mappings();
    let find =
        |dest, port, hostname, protocol| mappings.find(dest, port, hostname, protocol).unwrap();
    assert_eq!(find("10.1.2.3:80", 80, None, Protocol::Http), Some("ten"));
    assert_eq!(
        find("10.1.2.3:5432", 0, None, Protocol::Tcp),
        Some("postgres")
    );
    assert_eq!(find("10.1.2.3:5433", 0, None, Protocol::Tcp), None);
    assert_eq!(find("11.1.2.3:80", 80, None, Protocol::Http), None);
    // An IPv4 range holds the IPv4-mapped IPv6 form of its addresses.
    assert_eq!(
        find("[::ffff:10.1.2.3]:80", 80, None, Protocol::Http),
        Some("ten")
    );
}

#[test]
fn find_fails_for_a_range_and_a_hostname_destination() {
    let mut mappings = Mappings::default();
    mappings.upsert(cidr("10.0.0.0/8"), 0, Protocol::Http, ());
    let error = mappings
        .find("example.com:80", 80, None, Protocol::Http)
        .unwrap_err();
    assert!(error.description().contains("Invalid IP address"));
}

#[test]
fn ipv6_range_does_not_hold_ipv4() {
    let mut mappings = Mappings::default();
    mappings.upsert(cidr("::/0"), 0, Protocol::Tcp, ());
    assert_eq!(
        mappings.find("10.0.0.1:1", 0, None, Protocol::Tcp).unwrap(),
        None
    );
    assert_eq!(
        mappings.find("[::1]:1", 0, None, Protocol::Tcp).unwrap(),
        Some(())
    );
}

#[test]
fn upsert_replaces_the_same_mapping() {
    let mut mappings = Mappings::default();
    mappings.upsert(hostname("a.test"), 80, Protocol::Http, 1);
    mappings.upsert(hostname("a.test"), 81, Protocol::Http, 2);
    mappings.upsert(hostname("a.test"), 80, Protocol::Https, 3);
    mappings.upsert(hostname("a.test"), 80, Protocol::Http, 4);
    assert_eq!(mappings.0.len(), 3);
    assert_eq!(
        mappings
            .find("x:80", 80, Some("a.test"), Protocol::Http)
            .unwrap(),
        Some(4)
    );
    assert!(mappings.has_https());
}

#[test]
fn dns_allow_hostnames() {
    let mut mappings = Mappings::default();
    assert!(mappings.dns_allow_hostnames().is_empty());
    mappings.upsert(hostname("a.test"), 80, Protocol::Http, ());
    mappings.upsert(hostname("a.test"), 443, Protocol::Https, ());
    mappings.upsert(hostname("b.test"), 0, Protocol::Http, ());
    assert_eq!(mappings.dns_allow_hostnames(), ["a.test", "b.test"]);
    mappings.upsert(cidr("10.0.0.0/8"), 0, Protocol::Tcp, ());
    mappings.upsert(hostname("c.test"), 0, Protocol::Http, ());
    assert_eq!(mappings.dns_allow_hostnames(), ["a.test", "b.test", "*"]);
}

fn routing_mappings() -> Mappings<&'static str> {
    let mut mappings = Mappings::default();
    mappings.upsert(hostname("secure.test"), 0, Protocol::Https, "tls");
    mappings.upsert(hostname("plain.test"), 0, Protocol::Http, "http");
    mappings.upsert(hostname("both.test"), 0, Protocol::Https, "both-tls");
    mappings.upsert(hostname("both.test"), 0, Protocol::Http, "both-http");
    mappings.upsert(cidr("10.0.0.0/8"), 5432, Protocol::Tcp, "postgres");
    mappings
}

fn intercept(protocol: Protocol, default_port: u16, hostname: &str) -> Route<&'static str> {
    Route::Intercept {
        protocol,
        default_port,
        hostname: Some(hostname.to_owned()),
    }
}

#[test]
fn route_intercepts_a_matching_tls_hint() {
    let mappings = routing_mappings();
    let route = mappings
        .route("1.2.3.4:443", Some("secure.test"), None)
        .unwrap();
    assert_eq!(route, intercept(Protocol::Https, 443, "secure.test"));
    assert_eq!(route.status(), (200, "OK"));
}

#[test]
fn route_intercepts_a_matching_http_hint() {
    let mappings = routing_mappings();
    let route = mappings
        .route("1.2.3.4:80", None, Some("plain.test"))
        .unwrap();
    assert_eq!(route, intercept(Protocol::Http, 80, "plain.test"));
    assert_eq!(route.status(), (200, "OK"));
}

#[test]
fn route_checks_the_tls_hint_before_the_http_hint() {
    let mappings = routing_mappings();
    assert_eq!(
        mappings
            .route("1.2.3.4:443", Some("both.test"), Some("both.test"))
            .unwrap(),
        intercept(Protocol::Https, 443, "both.test")
    );
    // A TLS hint that matches nothing passes the tunnel through, even when the HTTP hint
    // would match: the bytes are TLS, which an HTTP mapping cannot read.
    assert_eq!(
        mappings
            .route("1.2.3.4:443", Some("other.test"), Some("plain.test"))
            .unwrap(),
        Route::PassThrough
    );
}

#[test]
fn route_passes_through_an_unmatched_hint() {
    let mappings = routing_mappings();
    // Even to an address a TCP mapping covers: the hint says the stream is TLS or HTTP.
    let route = mappings
        .route("10.1.2.3:5432", None, Some("other.test"))
        .unwrap();
    assert_eq!(route, Route::PassThrough);
    assert_eq!(route.status(), (202, "Accepted"));
}

#[test]
fn route_hands_hintless_tcp_to_its_mapping() {
    let mappings = routing_mappings();
    let route = mappings.route("10.1.2.3:5432", None, None).unwrap();
    assert_eq!(route, Route::Tcp("postgres"));
    // 202: the sidecar must not read the stream as HTTP, since it may carry TLS.
    assert_eq!(route.status(), (202, "Accepted"));
    assert_eq!(
        mappings.route("10.1.2.3:5433", None, None).unwrap(),
        Route::PassThrough
    );
}

#[test]
fn route_uses_the_default_port_of_the_hint() {
    let mut mappings = Mappings::default();
    mappings.upsert(hostname("a.test"), 443, Protocol::Https, "tls-443");
    mappings.upsert(hostname("a.test"), 80, Protocol::Http, "http-80");
    assert_eq!(
        mappings.route("1.2.3.4", Some("a.test"), None).unwrap(),
        intercept(Protocol::Https, 443, "a.test")
    );
    assert_eq!(
        mappings.route("1.2.3.4", None, Some("a.test")).unwrap(),
        intercept(Protocol::Http, 80, "a.test")
    );
}

#[test]
fn route_without_mappings_passes_through() {
    let mappings: Mappings<()> = Mappings::default();
    assert_eq!(
        mappings.route("1.2.3.4:443", Some("a.test"), None).unwrap(),
        Route::PassThrough
    );
    assert_eq!(
        mappings.route("1.2.3.4:22", None, None).unwrap(),
        Route::PassThrough
    );
}
