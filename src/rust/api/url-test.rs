// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn test_domain_to_ascii() {
    let util = UrlUtil {};
    assert_eq!(util.domain_to_ascii(String::new()).unwrap(), "");
    assert_eq!(
        util.domain_to_ascii("español.com".to_owned()).unwrap(),
        "xn--espaol-zwa.com"
    );
    assert_eq!(
        util.domain_to_ascii("理容ナカムラ.com".to_owned()).unwrap(),
        "xn--lck1c3crb1723bpq4a.com"
    );
}

#[test]
fn test_domain_to_unicode() {
    let util = UrlUtil {};
    assert_eq!(util.domain_to_unicode(String::new()).unwrap(), "");
    assert_eq!(
        util.domain_to_unicode("xn--espaol-zwa.com".to_owned())
            .unwrap(),
        "español.com"
    );
    assert_eq!(
        util.domain_to_unicode("xn--lck1c3crb1723bpq4a.com".to_owned())
            .unwrap(),
        "理容ナカムラ.com"
    );
}

#[test]
fn test_to_ascii() {
    let util = UrlUtil {};
    assert_eq!(
        util.to_ascii("meßagefactory.ca".to_owned()),
        "xn--meagefactory-m9a.ca"
    );
}

#[test]
fn test_format_strips_components() {
    let util = UrlUtil {};
    let href = "http://user:pass@example.com/a?b=c#d".to_owned();

    // Everything kept.
    assert_eq!(
        util.format(href.clone(), true, false, true, true).unwrap(),
        "http://user:pass@example.com/a?b=c#d"
    );
    // Drop hash.
    assert_eq!(
        util.format(href.clone(), false, false, true, true).unwrap(),
        "http://user:pass@example.com/a?b=c"
    );
    // Drop search.
    assert_eq!(
        util.format(href.clone(), true, false, false, true).unwrap(),
        "http://user:pass@example.com/a#d"
    );
    // Drop auth.
    assert_eq!(
        util.format(href, true, false, true, false).unwrap(),
        "http://example.com/a?b=c#d"
    );
}

#[test]
fn test_format_unicode() {
    let util = UrlUtil {};
    // Unicode hostname must survive serialization for a special (http) scheme.
    assert_eq!(
        util.format(
            "http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b#c".to_owned(),
            true,
            true,
            true,
            true
        )
        .unwrap(),
        "http://user:pass@理容ナカムラ.com/a?a=b#c"
    );
    // Port is preserved when splicing the unicode hostname.
    assert_eq!(
        util.format(
            "http://user:pass@xn--0zwm56d.com:8080/path".to_owned(),
            true,
            true,
            true,
            true
        )
        .unwrap(),
        "http://user:pass@测试.com:8080/path"
    );
}

#[test]
fn test_format_unicode_ipv6() {
    let util = UrlUtil {};
    // IPv6 literals are bracketed in the href but `hostname()` strips the
    // brackets. The unicode splice must preserve the brackets (and the
    // trailing port) rather than corrupting the address.
    assert_eq!(
        util.format(
            "http://[2001:db8::1]:8080/path".to_owned(),
            true,
            true,
            true,
            true
        )
        .unwrap(),
        "http://[2001:db8::1]:8080/path"
    );
    // IPv6 with credentials (so `host_start` from components points at `@`).
    assert_eq!(
        util.format(
            "http://user:pass@[::1]/path".to_owned(),
            true,
            true,
            true,
            true
        )
        .unwrap(),
        "http://user:pass@[::1]/path"
    );
}

#[test]
fn test_format_invalid() {
    let util = UrlUtil {};
    assert!(
        util.format("not a url".to_owned(), true, false, true, true)
            .is_err()
    );
}

#[test]
fn test_canonicalize_ip() {
    let util = UrlUtil {};
    // Already-canonical IPv4 is returned unchanged.
    assert_eq!(
        util.canonicalize_ip("192.168.1.1".to_owned()),
        "192.168.1.1"
    );
    // IPv6 is canonicalized (zero-compressed / lower-cased).
    assert_eq!(
        util.canonicalize_ip("2001:0DB8:0000:0000:0000:0000:0000:0001".to_owned()),
        "2001:db8::1"
    );
    // Invalid input (including leading-zero IPv4 octets, which Rust's
    // IpAddr parser rejects) yields an empty string.
    assert_eq!(util.canonicalize_ip("192.168.000.001".to_owned()), "");
    assert_eq!(util.canonicalize_ip("not-an-ip".to_owned()), "");
}
