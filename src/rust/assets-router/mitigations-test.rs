use super::*;

fn url_with_param(pathname: &str, name: &str, value: &str) -> Url {
    let mut url = Url::parse(&format!("https://example.com{pathname}")).unwrap();
    url.query_pairs_mut().append_pair(name, value);
    url
}

fn request<'a>(url: &'a Url, sec_fetch_dest: Option<&'a str>) -> Request<'a> {
    Request {
        url,
        method: Method::Get,
        sec_fetch_mode: None,
        sec_fetch_dest,
    }
}

/// Whether a response to a `_next/image` request is replaced by a 403.
fn next_image_blocked(
    pathname: &str,
    sec_fetch_dest: Option<&str>,
    source: &str,
    status: u16,
    content_type: &str,
) -> bool {
    let url = url_with_param(pathname, "url", source);
    filters_next_image_response(&request(&url, sec_fetch_dest))
        && should_block_image_response(status, content_type.as_bytes())
}

fn assert_next_image_blocked(pathname: &str) {
    for content_type in [
        "",
        "text/html",
        "image/jpg,text/plain",
        "image/jpg,text/html",
        "text/plain,text/html",
    ] {
        assert!(
            next_image_blocked(
                pathname,
                None,
                "https://example.com/image.jpg",
                200,
                content_type
            ),
            "{content_type}"
        );
    }
}

fn assert_next_image_allowed(
    pathname: &str,
    sec_fetch_dest: Option<&str>,
    source: &str,
    status: u16,
    content_type: Option<&str>,
) {
    assert!(!next_image_blocked(
        pathname,
        sec_fetch_dest,
        source,
        status,
        content_type.unwrap_or_default()
    ));
}

#[test]
fn next_image_blocks_html_content_type_at_subpath() {
    assert_next_image_blocked("/some/subpath/_next/image");
}

#[test]
fn next_image_allows_image_fetch_dest_at_subpath() {
    assert_next_image_allowed(
        "/some/subpath/_next/image",
        Some("image"),
        "https://example.com/image.jpg",
        200,
        Some("image/jpeg"),
    );
}

#[test]
fn next_image_allows_image_content_type_at_subpath() {
    assert_next_image_allowed(
        "/some/subpath/_next/image",
        None,
        "https://example.com/image.jpg",
        200,
        Some("image/jpeg"),
    );
}

#[test]
fn next_image_allows_text_plain_at_subpath() {
    assert_next_image_allowed(
        "/some/subpath/_next/image",
        None,
        "https://example.com/image.jpg",
        200,
        Some("text/plain"),
    );
}

#[test]
fn next_image_allows_text_plain_with_charset_at_subpath() {
    assert_next_image_allowed(
        "/some/subpath/_next/image",
        None,
        "https://example.com/image.jpg",
        200,
        Some("text/plain;charset=UTF-8"),
    );
}

#[test]
fn next_image_image_header_trumps_html_body_at_subpath() {
    assert_next_image_allowed(
        "/some/subpath/_next/image",
        Some("image"),
        "https://example.com/image.jpg",
        200,
        Some("text/html"),
    );
}

#[test]
fn next_image_allows_local_urls_at_subpath() {
    assert_next_image_allowed(
        "/some/subpath/_next/image",
        None,
        "/local/image.jpg",
        200,
        Some("text/html"),
    );
}

#[test]
fn next_image_allows_304_status_at_subpath() {
    assert_next_image_allowed(
        "/some/subpath/_next/image",
        None,
        "https://example.com/image.jpg",
        304,
        Some("text/html"),
    );
}

#[test]
fn next_image_blocks_html_content_type_at_root() {
    assert_next_image_blocked("/_next/image");
}

#[test]
fn next_image_allows_image_fetch_dest_at_root() {
    assert_next_image_allowed(
        "/_next/image",
        Some("image"),
        "https://example.com/image.jpg",
        200,
        Some("image/jpeg"),
    );
}

#[test]
fn next_image_allows_image_content_type_at_root() {
    assert_next_image_allowed(
        "/_next/image",
        None,
        "https://example.com/image.jpg",
        200,
        Some("image/jpeg"),
    );
}

#[test]
fn next_image_allows_text_plain_at_root() {
    assert_next_image_allowed(
        "/_next/image",
        None,
        "https://example.com/image.jpg",
        200,
        Some("text/plain"),
    );
}

#[test]
fn next_image_allows_text_plain_with_charset_at_root() {
    assert_next_image_allowed(
        "/_next/image",
        None,
        "https://example.com/image.jpg",
        200,
        Some("text/plain;charset=UTF-8"),
    );
}

#[test]
fn next_image_image_header_trumps_html_body_at_root() {
    assert_next_image_allowed(
        "/_next/image",
        Some("image"),
        "https://example.com/image.jpg",
        200,
        Some("text/html"),
    );
}

#[test]
fn next_image_allows_local_urls_at_root() {
    assert_next_image_allowed(
        "/_next/image",
        None,
        "/local/image.jpg",
        200,
        Some("text/html"),
    );
}

#[test]
fn next_image_allows_304_status_at_root() {
    assert_next_image_allowed(
        "/_next/image",
        None,
        "https://example.com/image.jpg",
        304,
        Some("text/html"),
    );
}

// Upstream only checks the response when `url` is non-empty.
#[test]
fn next_image_ignores_an_empty_url() {
    assert_next_image_allowed("/_next/image", None, "", 200, Some("text/html"));
}

#[test]
fn next_image_checks_any_method_but_get() {
    let url = url_with_param("/_next/image", "url", "https://example.com/image.jpg");
    let mut request = request(&url, Some("image"));
    assert!(!filters_next_image_response(&request));
    request.method = Method::Head;
    assert!(filters_next_image_response(&request));
}

#[test]
fn non_utf8_content_type_is_blocked() {
    assert!(should_block_image_response(200, b"image/\xff"));
}

fn image_href_blocked(href: &str, sec_fetch_dest: Option<&str>) -> bool {
    let url = url_with_param("/_image", "href", href);
    blocks_image_href(&request(&url, sec_fetch_dest))
}

#[test]
fn image_blocks_protocol_relative_different_hostname() {
    assert!(image_href_blocked("//evil.com/ssrf", None));
}

#[test]
fn image_blocks_whatwg_backslash_hostname_bypass() {
    assert!(image_href_blocked(r"//evil.com\@example.com/image", None));
}

#[test]
fn image_allows_same_hostname_protocol_relative() {
    assert!(!image_href_blocked("//example.com/image.jpg", None));
    assert!(!image_href_blocked("//EXAMPLE.com/image.jpg", None));
}

#[test]
fn image_allows_different_hostname_with_image_fetch_dest() {
    assert!(!image_href_blocked(
        "//another.com/image.jpg",
        Some("image")
    ));
}

#[test]
fn image_allows_an_href_that_is_not_protocol_relative_or_does_not_parse() {
    assert!(!image_href_blocked("/local/image.jpg", None));
    assert!(!image_href_blocked("//", None));
    assert!(!image_href_blocked("//[", None));
}

fn assert_redirect(raw_url: &str, normalized_url: &str) {
    let url = Url::parse(raw_url).unwrap();
    assert!(is_cdn_cgi_backslash_bypass(raw_url, &url));
    assert_eq!(url.as_str(), normalized_url);
}

#[test]
fn cdn_cgi_backslash_redirects_image() {
    assert_redirect(
        r"https://example.com/cdn-cgi\image/q=75/https://evil.com/ssrf",
        "https://example.com/cdn-cgi/image/q=75/https://evil.com/ssrf",
    );
}

#[test]
fn cdn_cgi_backslash_redirects_next_cache() {
    assert_redirect(
        r"https://example.com/cdn-cgi\_next_cache/some-data",
        "https://example.com/cdn-cgi/_next_cache/some-data",
    );
}

#[test]
fn cdn_cgi_backslash_redirects_arbitrary_subpath() {
    assert_redirect(
        r"https://example.com/cdn-cgi\something-else/path",
        "https://example.com/cdn-cgi/something-else/path",
    );
}

#[test]
fn cdn_cgi_backslash_redirects_with_query() {
    assert_redirect(
        r"https://example.com/cdn-cgi\something-else/path?value=/cdn-cgi/param=foo",
        "https://example.com/cdn-cgi/something-else/path?value=/cdn-cgi/param=foo",
    );
}

fn assert_no_redirect(raw_url: &str, pathname: &str) {
    let url = Url::parse(raw_url).unwrap();
    assert_eq!(url.path(), pathname);
    assert!(!is_cdn_cgi_backslash_bypass(raw_url, &url));
}

#[test]
fn cdn_cgi_no_interference_forward_slash() {
    assert_no_redirect(
        "https://example.com/cdn-cgi/image/q=75/https://other.com/image.jpg",
        "/cdn-cgi/image/q=75/https://other.com/image.jpg",
    );
}

#[test]
fn cdn_cgi_no_interference_escaped_backslash() {
    assert_no_redirect(
        "https://example.com/cdn-cgi%5Cimage/q=75/https://other.com/image.jpg",
        "/cdn-cgi%5Cimage/q=75/https://other.com/image.jpg",
    );
}

#[test]
fn cdn_cgi_no_interference_escaped_forward_slash() {
    assert_no_redirect(
        "https://example.com/cdn-cgi%2Fimage/q=75/https://other.com/image.jpg",
        "/cdn-cgi%2Fimage/q=75/https://other.com/image.jpg",
    );
}

#[test]
fn cdn_cgi_no_interference_non_cdn_cgi() {
    assert_no_redirect("https://example.com/some-page", "/some-page");
}

#[test]
fn cdn_cgi_no_interference_cdn_cgi_in_query() {
    assert_no_redirect(
        r"https://example.com/some-page?redirect=/cdn-cgi\image",
        "/some-page",
    );
}
