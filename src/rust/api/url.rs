// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::net::IpAddr;
use std::str::FromStr;

use ada_url::Idna;
use ada_url::Url;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum UrlError {
    #[error("{0}")]
    ParseFailed(&'static str),
}

impl From<UrlError> for jsg::Error {
    fn from(val: UrlError) -> Self {
        match val {
            UrlError::ParseFailed(msg) => Self::new_error(msg),
        }
    }
}

/// Implementation shared by `domainToASCII` and `domainToUnicode`.
///
/// Returns the parsed (ASCII / punycode) hostname for the given domain, or
/// `None` if the domain is empty or fails to parse as a hostname.
///
/// # Errors
/// `UrlError::ParseFailed` if the placeholder URL fails to parse (should never
/// happen in practice — it mirrors the `JSG_REQUIRE` in the C++ implementation).
fn get_hostname(domain: &str) -> Result<Option<String>, UrlError> {
    if domain.is_empty() {
        return Ok(None);
    }

    // It is important to have an initial value that contains a special scheme.
    // Since it will change the implementation of `set_hostname` according to the
    // URL spec.
    let mut out =
        Url::parse("ws://x", None).map_err(|_| UrlError::ParseFailed("URL parsing failed"))?;
    match out.set_hostname(Some(domain)) {
        Ok(()) => Ok(Some(out.hostname().to_owned())),
        Err(()) => Ok(None),
    }
}

#[jsg_resource]
pub struct UrlUtil;

#[jsg_resource]
impl UrlUtil {
    #[must_use]
    pub fn new() -> jsg::Rc<Self> {
        jsg::Rc::new(Self {})
    }

    /// # Errors
    /// `UrlError::ParseFailed` (see `get_hostname`).
    #[jsg_method(name = "domainToASCII")]
    pub fn domain_to_ascii(&self, domain: String) -> Result<String, UrlError> {
        Ok(get_hostname(&domain)?.unwrap_or_default())
    }

    /// # Errors
    /// `UrlError::ParseFailed` (see `get_hostname`).
    #[jsg_method(name = "domainToUnicode")]
    pub fn domain_to_unicode(&self, domain: String) -> Result<String, UrlError> {
        Ok(match get_hostname(&domain)? {
            Some(hostname) => Idna::unicode(&hostname),
            None => String::new(),
        })
    }

    #[jsg_method(name = "toASCII")]
    pub fn to_ascii(&self, url: String) -> String {
        Idna::ascii(&url)
    }

    /// # Errors
    /// `UrlError::ParseFailed` if the input cannot be parsed as a URL.
    #[jsg_method(name = "format")]
    pub fn format(
        &self,
        input: String,
        hash: bool,
        unicode: bool,
        search: bool,
        auth: bool,
    ) -> Result<String, UrlError> {
        let mut out = Url::parse(input.as_str(), None)
            .map_err(|_| UrlError::ParseFailed("Failed to parse URL"))?;

        if !hash {
            out.set_hash(None);
        }

        if !search {
            out.set_search(None);
        }

        if !auth {
            // Clearing both the username and the password removes the userinfo
            // section entirely, matching the C++ implementation which assigns
            // empty strings to both fields. The setters can refuse on URLs that
            // cannot have credentials, in which case there is nothing to clear.
            let _ = out.set_username(Some(""));
            let _ = out.set_password(Some(""));
        }

        if unicode && out.has_hostname() {
            // The C++ implementation assigns the IDNA Unicode form directly to
            // the host field, bypassing the host parser which would otherwise
            // re-encode it back to ASCII/punycode for special schemes. The Rust
            // binding only exposes parser-backed setters, so we instead splice
            // the Unicode hostname into the serialized href.
            //
            // `host_end` reliably marks the end of the (ASCII/punycode) host in
            // the href, but `host_start` from `components()` points at the `@`
            // separator when credentials are present, so we derive the start from
            // the known host length instead. `hostname()` matches the WHATWG
            // `URL.hostname` serialization, so IPv6 literals keep their brackets
            // (`[::1]`) — the same brackets present in the href — and the length
            // math needs no special-casing. The host is always ASCII in the href,
            // so these offsets fall on valid UTF-8 boundaries; we nonetheless use
            // checked arithmetic and `get()` to avoid any chance of a panic.
            let hostname = out.hostname();
            let unicode_host = Idna::unicode(hostname);
            let host_end = out.components().host_end as usize;
            let href = out.href();

            let host_start = host_end
                .checked_sub(hostname.len())
                .ok_or(UrlError::ParseFailed("URL host offset underflow"))?;
            let prefix = href
                .get(..host_start)
                .ok_or(UrlError::ParseFailed("URL host offset out of bounds"))?;
            let suffix = href
                .get(host_end..)
                .ok_or(UrlError::ParseFailed("URL host offset out of bounds"))?;

            return Ok(format!("{prefix}{unicode_host}{suffix}"));
        }

        Ok(out.href().to_owned())
    }

    // We return an empty string if the input is not a valid IP address. This
    // mirrors `workerd::rust::net::canonicalize_ip`, reimplemented here to avoid
    // a cross-crate FFI dependency now that the caller is itself Rust.
    #[jsg_method(name = "canonicalizeIp")]
    pub fn canonicalize_ip(&self, input: String) -> String {
        IpAddr::from_str(&input).map_or(String::new(), |ip| ip.to_string())
    }
}

#[cfg(test)]
mod tests {
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
}
