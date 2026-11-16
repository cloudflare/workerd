use url::Url;

use super::super::Method;
use super::super::intent::HtmlHandling;
use super::super::intent::NotFoundHandling;
use super::super::rules::Redirect;
use super::*;

fn rules(rules: &[&str]) -> Vec<Regex> {
    globs(
        &rules
            .iter()
            .map(|rule| (*rule).to_owned())
            .collect::<Vec<_>>(),
    )
}

fn config(
    has_user_worker: bool,
    invoke_ahead: bool,
    user_rules: &[&str],
    asset_rules: &[&str],
) -> RoutingConfig {
    RoutingConfig {
        has_user_worker,
        invoke_user_worker_ahead_of_assets: invoke_ahead,
        user_worker_rules: rules(user_rules),
        asset_worker_rules: rules(asset_rules),
        intent: IntentConfig {
            html_handling: HtmlHandling::AutoTrailingSlash,
            not_found_handling: NotFoundHandling::None,
            has_static_routing: !user_rules.is_empty() || !asset_rules.is_empty(),
            navigation_prefers_asset_serving: false,
        },
        base_path: BasePath::default(),
        redirects: Redirects::new([], []),
        header_rules: RulesMatcher::new([]),
    }
}

fn config_with_base_path(
    has_user_worker: bool,
    user_rules: &[&str],
    asset_rules: &[&str],
) -> RoutingConfig {
    RoutingConfig {
        base_path: BasePath::normalize("/subpath").unwrap(),
        ..config(has_user_worker, false, user_rules, asset_rules)
    }
}

struct TestRequest<'a> {
    pathname: &'a str,
    method: Method,
    sec_fetch_dest: Option<&'a str>,
}

fn get(pathname: &str) -> TestRequest<'_> {
    TestRequest {
        pathname,
        method: Method::Get,
        sec_fetch_dest: None,
    }
}

fn route(
    request: &TestRequest<'_>,
    config: &RoutingConfig,
    assets: &[&str],
) -> Result<Route, String> {
    let url = Url::parse(&format!("https://example.com{}", request.pathname)).unwrap();
    config.route(
        config.rule_route(url.path()),
        &Request {
            url: &url,
            method: request.method,
            sec_fetch_mode: None,
            sec_fetch_dest: request.sec_fetch_dest,
        },
        &|path: &str| assets.contains(&path).then(|| path.to_owned()),
    )
}

fn decide_with_assets(
    pathname: &str,
    config: &RoutingConfig,
    assets: &[&str],
) -> Result<Route, String> {
    route(&get(pathname), config, assets)
}

const USER_WORKER: Result<Route, String> = Ok(Route::UserWorker {
    filter_image_response: false,
    block_image_href: false,
});

/// The file at `path`, served.
#[expect(
    clippy::unnecessary_wraps,
    reason = "compared with the result of routing"
)]
fn asset(path: &str) -> Result<Route, String> {
    Ok(Route::Asset(Asset {
        path: path.into(),
        etag: path.into(),
        status: 200,
        not_found: false,
    }))
}

/// The assets' 404.
const NOT_FOUND: Result<Route, String> = Ok(Route::Respond(Response {
    status: 404,
    headers: vec![],
    body: "",
}));

#[expect(
    clippy::unnecessary_wraps,
    reason = "compared with the result of routing"
)]
fn redirect(status: u16, location: &str) -> Result<Route, String> {
    Ok(Route::Respond(Response {
        status,
        headers: vec![ResponseHeader {
            name: "Location".into(),
            value: location.into(),
        }],
        body: "",
    }))
}

#[test]
fn invoke_ahead_without_user_worker_errors() {
    assert_eq!(
        decide_with_assets("/asset", &config(false, true, &[], &[]), &["/asset"]),
        Err("Fetch for user worker without having a user worker binding".into())
    );
}

#[test]
fn assets_only_miss_routes_to_asset_worker() {
    assert_eq!(
        decide_with_assets("/missing", &config(false, false, &[], &[]), &["/asset"]),
        NOT_FOUND
    );
}

#[test]
fn assets_only_hit_routes_to_asset_worker() {
    assert_eq!(
        decide_with_assets("/asset", &config(false, false, &[], &[]), &["/asset"]),
        asset("/asset")
    );
}

#[test]
fn invoke_ahead_routes_to_user() {
    assert_eq!(
        decide_with_assets("/asset", &config(true, true, &[], &[]), &["/asset"]),
        USER_WORKER
    );
}

#[test]
fn asset_hit_routes_to_asset() {
    assert_eq!(
        decide_with_assets("/asset", &config(true, false, &[], &[]), &["/asset"]),
        asset("/asset")
    );
}

#[test]
fn static_routing_user_worker_include_matches() {
    assert_eq!(
        decide_with_assets(
            "/api/x",
            &config(true, false, &["/api/*"], &[]),
            &["/api/x"]
        ),
        USER_WORKER
    );
}

#[test]
fn static_routing_asset_worker_exclude_matches() {
    assert_eq!(
        decide_with_assets("/public/x", &config(true, true, &[], &["/public/*"]), &[]),
        NOT_FOUND
    );
}

#[test]
fn static_routing_asset_worker_wins_over_user_worker() {
    assert_eq!(
        decide_with_assets(
            "/overlap",
            &config(true, false, &["/overlap"], &["/overlap"]),
            &[]
        ),
        NOT_FOUND
    );
}

#[test]
fn static_routing_missing_user_worker_rules_wc_5014() {
    assert_eq!(
        decide_with_assets(
            "/asset",
            &config(true, false, &[], &["/other"]),
            &["/asset"]
        ),
        asset("/asset")
    );
}

#[test]
fn static_routing_miss_falls_through_to_asset_hit() {
    assert_eq!(
        decide_with_assets(
            "/asset",
            &config(true, false, &["/api/*"], &[]),
            &["/asset"]
        ),
        asset("/asset")
    );
}

#[test]
fn static_routing_miss_falls_through_to_user_worker() {
    assert_eq!(
        decide_with_assets("/missing", &config(true, false, &["/api/*"], &[]), &[]),
        USER_WORKER
    );
}

#[test]
fn html_canonicalization_redirect_is_native() {
    assert_eq!(
        decide_with_assets("/foo/", &config(true, false, &[], &[]), &["/foo.html"]),
        redirect(307, "/foo")
    );
}

#[test]
fn unsupported_method_on_an_asset_is_native() {
    let request = TestRequest {
        method: Method::Other,
        ..get("/asset")
    };
    assert_eq!(
        route(&request, &config(true, false, &[], &[]), &["/asset"]),
        Ok(Route::Respond(Response {
            status: 405,
            headers: vec![],
            body: "",
        }))
    );
    assert_eq!(
        route(&request, &config(true, false, &[], &[]), &[]),
        USER_WORKER
    );
}

// The assets answer with `not_found_handling`, which `canFetch` leaves out.
#[test]
fn not_found_handling_answers_only_what_reaches_the_assets() {
    let spa = |has_user_worker| RoutingConfig {
        intent: IntentConfig {
            not_found_handling: NotFoundHandling::SinglePageApplication,
            ..IntentConfig::default()
        },
        ..config(has_user_worker, false, &[], &[])
    };
    let assets = ["/index.html", "/about.html"];
    assert_eq!(
        decide_with_assets("/about", &spa(true), &assets),
        asset("/about.html")
    );
    assert_eq!(
        decide_with_assets("/missing", &spa(true), &assets),
        USER_WORKER
    );
    assert_eq!(
        decide_with_assets("/missing", &spa(false), &assets),
        Ok(Route::Asset(Asset {
            path: "/index.html".into(),
            etag: "/index.html".into(),
            status: 200,
            not_found: true,
        }))
    );
}

fn config_with_redirects(has_user_worker: bool, invoke_ahead: bool) -> RoutingConfig {
    RoutingConfig {
        redirects: Redirects::new(
            [(
                "/api/old".to_owned(),
                (
                    1,
                    Redirect {
                        status: 301,
                        to: "/api/new".into(),
                    },
                ),
            )],
            [
                (
                    "/blog/*".to_owned(),
                    Redirect {
                        status: 302,
                        to: "/posts/:splat".into(),
                    },
                ),
                (
                    "/users/*".to_owned(),
                    Redirect {
                        status: 200,
                        to: "/users/id".into(),
                    },
                ),
            ],
        ),
        header_rules: RulesMatcher::new([(
            "/blog/*".to_owned(),
            HeaderRule {
                set: vec![("X-Blog".into(), ":splat".into())],
                unset: vec![],
            },
        )]),
        ..config(has_user_worker, invoke_ahead, &["/api/*"], &[])
    }
}

// Upstream applies `_redirects` in the asset worker, so only to a request that is routed to
// the assets; a rule that matches makes `canFetch` true.
#[test]
fn redirect_rules_apply_to_requests_routed_to_assets() {
    let config = config_with_redirects(true, false);
    assert_eq!(
        decide_with_assets("/blog/hello", &config, &[]),
        Ok(Route::Respond(Response {
            status: 302,
            headers: vec![
                ResponseHeader {
                    name: "Location".into(),
                    value: "/posts/hello".into(),
                },
                ResponseHeader {
                    name: "X-Blog".into(),
                    value: "hello".into(),
                },
            ],
            body: "",
        }))
    );
    assert_eq!(
        decide_with_assets("/blog/hello", &config_with_redirects(false, false), &[]),
        decide_with_assets("/blog/hello", &config, &[])
    );
}

#[test]
fn redirect_rules_do_not_apply_to_requests_routed_to_the_user_worker() {
    assert_eq!(
        decide_with_assets("/api/old", &config_with_redirects(true, false), &[]),
        USER_WORKER
    );
    assert_eq!(
        decide_with_assets("/blog/hello", &config_with_redirects(true, true), &[]),
        USER_WORKER
    );
}

#[test]
fn rewrite_rule_claims_the_request_for_the_assets() {
    let config = config_with_redirects(true, false);
    assert_eq!(
        decide_with_assets("/users/12345", &config, &["/users/id.html"]),
        asset("/users/id.html")
    );
    assert_eq!(decide_with_assets("/users/12345", &config, &[]), NOT_FOUND);
}

#[test]
fn base_path_asset_is_looked_up_relative_to_the_asset_directory() {
    let config = config_with_base_path(true, &[], &[]);
    assert_eq!(
        decide_with_assets("/subpath/asset", &config, &["/asset"]),
        asset("/asset")
    );
    assert_eq!(
        decide_with_assets("/subpath/asset", &config, &["/subpath/asset"]),
        USER_WORKER
    );
}

#[test]
fn base_path_redirect_is_public() {
    assert_eq!(
        decide_with_assets(
            "/subpath/foo/",
            &config_with_base_path(true, &[], &[]),
            &["/foo.html"]
        ),
        redirect(307, "/subpath/foo")
    );
}

#[test]
fn off_base_path_request_routes_to_user_worker() {
    assert_eq!(
        decide_with_assets(
            "/asset",
            &config_with_base_path(true, &[], &[]),
            &["/asset"]
        ),
        USER_WORKER
    );
}

// Without a user worker the asset worker still receives the request, and answers that it has
// nothing to serve outside the base.
#[test]
fn off_base_path_request_without_user_worker_routes_to_asset_worker() {
    assert_eq!(
        decide_with_assets(
            "/asset",
            &config_with_base_path(false, &[], &[]),
            &["/asset"]
        ),
        NOT_FOUND
    );
}

#[test]
fn static_routing_matches_the_public_path() {
    let config = config_with_base_path(true, &["/subpath/api/*"], &["/subpath/public/*"]);
    assert_eq!(
        decide_with_assets("/subpath/api/x", &config, &["/api/x"]),
        USER_WORKER
    );
    assert_eq!(
        decide_with_assets("/subpath/public/x", &config, &[]),
        NOT_FOUND
    );
}

// Upstream: `/_image` and `_next/image` are only checked for a request that runs the user
// worker.
#[test]
fn image_mitigations_apply_to_the_user_worker_route() {
    let config = config(true, false, &[], &[]);
    assert_eq!(
        decide_with_assets("/_image?href=//evil.com/ssrf", &config, &[]),
        Ok(Route::UserWorker {
            filter_image_response: false,
            block_image_href: true,
        })
    );
    assert_eq!(
        decide_with_assets("/_image?href=//evil.com/ssrf", &config, &["/_image"]),
        asset("/_image")
    );
    assert_eq!(
        decide_with_assets("/_next/image?url=https://evil.com/image.jpg", &config, &[]),
        Ok(Route::UserWorker {
            filter_image_response: true,
            block_image_href: false,
        })
    );
    let from_img_tag = TestRequest {
        sec_fetch_dest: Some("image"),
        ..get("/_next/image?url=https://evil.com/image.jpg")
    };
    assert_eq!(route(&from_img_tag, &config, &[]), USER_WORKER);
}

fn matches(rules: &[&str], pathname: &str) -> bool {
    let owned: Vec<String> = rules.iter().map(|s| (*s).to_owned()).collect();
    globs(&owned).iter().any(|r| r.is_match(pathname))
}

// Upstream: rules-engine.test.ts `static routing rules > should return true for a request that
// matches` (L121).
#[test]
fn matches_exact_and_glob_paths() {
    assert!(matches(&["/some/path"], "/some/path"));
    assert!(matches(&["/some/*"], "/some/path"));
    assert!(matches(&["/no/match", "/some/*"], "/some/path"));
}

// Upstream: rules-engine.test.ts `static routing rules > should return false for a request that
// does not match` (L140).
#[test]
fn returns_false_on_no_match() {
    assert!(!matches(&["/some/path"], "/"));
    assert!(!matches(&["/some/*"], "/path"));
    assert!(!matches(&["/some/path", "/other/path"], "/path"));
    assert!(!matches(&[], "/some/path"));
}

// Upstream: rules-engine.test.ts `static routing rules > should ignore regex characters other
// than a glob` — rule "/" (L166).
#[test]
fn rule_root_matches_only_root() {
    assert!(matches(&["/"], "/"));
    assert!(!matches(&["/"], "/foo"));
}

// Upstream: rules-engine.test.ts `... ignore regex characters` — rule "/foo" (L187).
#[test]
fn rule_exact_path_no_prefix_or_subpath() {
    assert!(matches(&["/foo"], "/foo"));
    assert!(!matches(&["/foo"], "/"));
    assert!(!matches(&["/foo"], "/foo/"));
    assert!(!matches(&["/foo"], "/foo/bar"));
    assert!(!matches(&["/foo"], "/baz"));
    assert!(!matches(&["/foo"], "/baz/foo"));
    assert!(!matches(&["/foo"], "/foobar"));
}

// Upstream: rules-engine.test.ts `... ignore regex characters` — rule "/:placeholder" (L213).
#[test]
fn placeholder_treated_literally_not_as_placeholder() {
    assert!(!matches(&["/:placeholder"], "/foo"));
    assert!(matches(&["/:placeholder"], "/:placeholder"));
}

// Upstream: rules-engine.test.ts `... ignore regex characters` — rule "/foo*" (L223).
#[test]
fn glob_suffix_matches_prefixes_and_children() {
    assert!(matches(&["/foo*"], "/foo"));
    assert!(matches(&["/foo*"], "/foo/"));
    assert!(matches(&["/foo*"], "/foo/bar"));
    assert!(matches(&["/foo*"], "/foobar"));
    assert!(!matches(&["/foo*"], "/"));
    assert!(!matches(&["/foo*"], "/baz"));
    assert!(!matches(&["/foo*"], "/baz/foo"));
}

// Upstream: rules-engine.test.ts `... ignore regex characters` — further metacharacter block
// (L249). Rules containing regex-special characters must match them literally.
#[test]
fn more_regex_metacharacter_escapes() {
    // A dot is literal, not "any char".
    assert!(matches(&["/foo.html"], "/foo.html"));
    assert!(!matches(&["/foo.html"], "/fooXhtml"));
    // Parentheses / plus / question mark are literal.
    assert!(matches(&["/a(b)+c?"], "/a(b)+c?"));
    assert!(!matches(&["/a(b)+c?"], "/abbc"));
    // Glob still works alongside literal metacharacters.
    assert!(matches(&["/foo.*"], "/foo.bar"));
    assert!(!matches(&["/foo.*"], "/fooXbar"));
}
