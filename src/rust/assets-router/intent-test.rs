use std::collections::HashSet;

use url::Url;

use super::*;

/// Assets at `paths`, each its path for an `ETag`.
fn assets(paths: &[&str]) -> impl Fn(&str) -> Option<String> {
    let paths: HashSet<String> = paths.iter().map(|path| (*path).to_owned()).collect();
    move |path| paths.contains(path).then(|| path.to_owned())
}

/// An `ASSET`, whichever asset it is.
const ASSET: AssetResponse = AssetResponse::Asset(Asset {
    path: String::new(),
    etag: String::new(),
    status: 200,
    not_found: false,
});

/// The `_redirects` rule matching a test request.
#[derive(Clone, Copy, Debug)]
enum RedirectAction<'a> {
    None,
    Redirect,
    Rewrite(&'a str),
}

#[derive(Clone, Copy)]
struct TestRequest<'a> {
    pathname: &'a str,
    method: Method,
    sec_fetch_mode: Option<&'a str>,
    redirect: RedirectAction<'a>,
}

fn request(pathname: &str) -> TestRequest<'_> {
    TestRequest {
        pathname,
        method: Method::Get,
        sec_fetch_mode: None,
        redirect: RedirectAction::None,
    }
}

fn respond_under(
    base_path: &str,
    request: TestRequest<'_>,
    config: IntentConfig,
    exists: &impl Fn(&str) -> Option<String>,
) -> AssetResponse {
    let url = Url::parse(&format!("https://example.com{}", request.pathname)).unwrap();
    let redirect = match request.redirect {
        RedirectAction::None => None,
        RedirectAction::Redirect => Some(Redirect {
            status: 301,
            to: "/redirected".into(),
        }),
        RedirectAction::Rewrite(to) => Some(Redirect {
            status: 200,
            to: to.into(),
        }),
    };
    let response = respond(
        &Request {
            url: &url,
            method: request.method,
            sec_fetch_mode: request.sec_fetch_mode,
            sec_fetch_dest: None,
        },
        redirect.as_ref(),
        config,
        &BasePath::normalize(base_path).unwrap(),
        exists,
    );
    match response.unwrap() {
        AssetResponse::Asset(_) => ASSET,
        response => response,
    }
}

/// Mirrors upstream `canFetch`.
fn can_fetch(
    request: TestRequest<'_>,
    config: IntentConfig,
    exists: &impl Fn(&str) -> Option<String>,
) -> bool {
    let config = config.for_can_fetch(request.sec_fetch_mode);
    respond_under("/", request, config, exists) != AssetResponse::NoIntent
}

// Upstream: `canFetch > should return "true" if for exact and nearby assets with
// html_handling on` (L1083).
#[test]
fn canfetch_auto_trailing_slash_matches_nearby_paths() {
    let exists = assets(&["/foo.html"]);
    let config = IntentConfig::default();
    for pathname in ["/foo.html", "/foo", "/foo/"] {
        assert!(can_fetch(request(pathname), config, &exists), "{pathname}");
    }
}

// Upstream: `canFetch > should not consider 404s or SPAs` (L1125).
#[test]
fn canfetch_ignores_not_found_fallbacks() {
    let exists = assets(&["/404.html", "/index.html", "/foo.html"]);
    for not_found_handling in [
        NotFoundHandling::SinglePageApplication,
        NotFoundHandling::NotFoundPage,
    ] {
        let config = IntentConfig {
            not_found_handling,
            ..IntentConfig::default()
        };
        assert!(can_fetch(request("/foo"), config, &exists));
        assert!(!can_fetch(request("/bar"), config, &exists));
        assert!(can_fetch(request("/"), config, &exists));
        assert!(can_fetch(request("/404"), config, &exists));
    }
}

fn assert_static_routing(
    not_found_handling: NotFoundHandling,
    sec_fetch_mode: Option<&str>,
    navigation_prefers_asset_serving: bool,
) {
    let exists = assets(&["/404.html", "/index.html"]);
    let config = IntentConfig {
        not_found_handling,
        has_static_routing: true,
        navigation_prefers_asset_serving,
        ..IntentConfig::default()
    };
    for pathname in ["/foo", "/bar", "/", "/404"] {
        let mut request = request(pathname);
        request.sec_fetch_mode = sec_fetch_mode;
        assert!(can_fetch(request, config, &exists), "{pathname}");
    }
}

macro_rules! static_routing_test {
    ($name:ident, $nfh:expr, $header:expr, $flag:expr) => {
        #[test]
        fn $name() {
            assert_static_routing($nfh, $header, $flag);
        }
    };
}

static_routing_test!(
    canfetch_static_routing_forces_true_spa_no_header_flag_on,
    NotFoundHandling::SinglePageApplication,
    None,
    true
);
static_routing_test!(
    canfetch_static_routing_forces_true_spa_no_header_flag_off,
    NotFoundHandling::SinglePageApplication,
    None,
    false
);
static_routing_test!(
    canfetch_static_routing_forces_true_spa_navigate_flag_on,
    NotFoundHandling::SinglePageApplication,
    Some("navigate"),
    true
);
static_routing_test!(
    canfetch_static_routing_forces_true_spa_navigate_flag_off,
    NotFoundHandling::SinglePageApplication,
    Some("navigate"),
    false
);
static_routing_test!(
    canfetch_static_routing_forces_true_404_no_header_flag_on,
    NotFoundHandling::NotFoundPage,
    None,
    true
);
static_routing_test!(
    canfetch_static_routing_forces_true_404_no_header_flag_off,
    NotFoundHandling::NotFoundPage,
    None,
    false
);
static_routing_test!(
    canfetch_static_routing_forces_true_404_navigate_flag_on,
    NotFoundHandling::NotFoundPage,
    Some("navigate"),
    true
);
static_routing_test!(
    canfetch_static_routing_forces_true_404_navigate_flag_off,
    NotFoundHandling::NotFoundPage,
    Some("navigate"),
    false
);

// Upstream: `canFetch > should return true even for a bad method` (L1256).
#[test]
fn canfetch_method_agnostic() {
    let exists = assets(&["/foo.html"]);
    let mut req = request("/foo");
    req.method = Method::Other;
    assert!(can_fetch(req, IntentConfig::default(), &exists));
    req.pathname = "/bar";
    assert!(!can_fetch(req, IntentConfig::default(), &exists));
}

// Upstream: `canFetch > should return true for custom redirects without underlying assets`
// (L1286). Redirect matching is represented by RedirectAction rather than depending on the
// independently ported redirects module.
#[test]
fn canfetch_honors_redirects_without_asset() {
    let exists = assets(&["/404.html", "/does-exist"]);
    assert!(can_fetch(
        request("/does-exist"),
        IntentConfig::default(),
        &exists
    ));
    assert!(!can_fetch(
        request("/no-match"),
        IntentConfig::default(),
        &exists
    ));

    let mut req = request("/redirect");
    req.redirect = RedirectAction::Redirect;
    assert!(can_fetch(req, IntentConfig::default(), &exists));

    req.pathname = "/proxy-valid";
    req.redirect = RedirectAction::Rewrite("/does-exist");
    assert!(can_fetch(req, IntentConfig::default(), &exists));

    let missing = assets(&[]);
    req.pathname = "/proxy-invalid";
    req.redirect = RedirectAction::Rewrite("/no-match");
    assert!(can_fetch(req, IntentConfig::default(), &missing));
    assert!(can_fetch(
        req,
        IntentConfig {
            not_found_handling: NotFoundHandling::NotFoundPage,
            ..IntentConfig::default()
        },
        &missing,
    ));
}

// Upstream: `canFetch > assets_navigation_prefers_asset_serving > it.for(matrix)` (L1484).
// Upstream crosses seven compatibility-option rows (date × enable/disable flags) with the
// other dimensions. In the inlined runtime, workerd resolves the compatibility date and the
// enable/disable flags into a single bool before the router runs (see compatibility-date.capnp
// `assetsSecFetchModeNavigateHeaderPrefersAssetServing`, which carries `$compatEnableDate`
// and `$compatDisableFlag`), so those seven rows collapse to their resolved enabled values
// F,T,F,T,T,T,F. Each is crossed with not-found rows [default:F, none:F, 404:T, SPA:T], header
// rows [none:F, navigate:T, cors:F], and static-routing rows [false:F, true:T].
// 7 * 4 * 3 * 2 = exactly 168 rows.
#[test]
fn canfetch_navigation_matrix() {
    const NAVIGATION_ENABLED: [bool; 7] = [false, true, false, true, true, true, false];
    const NOT_FOUND: [(NotFoundHandling, bool); 4] = [
        (NotFoundHandling::None, false),
        (NotFoundHandling::None, false),
        (NotFoundHandling::NotFoundPage, true),
        (NotFoundHandling::SinglePageApplication, true),
    ];
    const HEADERS: [(Option<&str>, bool); 3] = [
        (None, false),
        (Some("navigate"), true),
        (Some("cors"), false),
    ];
    const STATIC_ROUTING: [(bool, bool); 2] = [(false, false), (true, true)];

    let exists = assets(&["/404.html", "/index.html", "/foo.html"]);
    let mut rows = 0;
    for navigation_prefers_asset_serving in NAVIGATION_ENABLED {
        for (not_found_handling, has_fallback) in NOT_FOUND {
            for (sec_fetch_mode, is_navigation) in HEADERS {
                for (has_static_routing, static_keeps_fallback) in STATIC_ROUTING {
                    rows += 1;
                    let config = IntentConfig {
                        not_found_handling,
                        has_static_routing,
                        navigation_prefers_asset_serving,
                        ..IntentConfig::default()
                    };
                    let expected = (static_keeps_fallback && has_fallback)
                        || (navigation_prefers_asset_serving && has_fallback && is_navigation);
                    let mut req = request("/bar");
                    req.sec_fetch_mode = sec_fetch_mode;
                    assert_eq!(
                        can_fetch(req, config, &exists),
                        expected,
                        "row {rows}: navigation={navigation_prefers_asset_serving}, nfh={not_found_handling:?}, mode={sec_fetch_mode:?}, static={has_static_routing}"
                    );
                    for pathname in ["/foo", "/", "/404"] {
                        req.pathname = pathname;
                        assert!(can_fetch(req, config, &exists), "row {rows}: {pathname}");
                    }
                }
            }
        }
    }
    assert_eq!(rows, 168);
}

#[test]
fn html_handling_modes_and_not_found_resolution() {
    let exists = assets(&["/foo.html", "/dir/index.html", "/404.html"]);
    let asset = |path: &str, status| {
        Some(Intent::Asset(Asset {
            path: path.into(),
            etag: path.into(),
            status,
            not_found: status == 404,
        }))
    };
    let redirect = |destination: &str| Some(Intent::Redirect(destination.to_owned()));

    let auto = IntentConfig::default();
    assert_eq!(get_intent("/foo", auto, &exists), asset("/foo.html", 200));
    assert_eq!(get_intent("/foo/", auto, &exists), redirect("/foo"));

    let force = IntentConfig {
        html_handling: HtmlHandling::ForceTrailingSlash,
        ..auto
    };
    assert_eq!(get_intent("/foo", force, &exists), redirect("/foo/"));

    let drop = IntentConfig {
        html_handling: HtmlHandling::DropTrailingSlash,
        ..auto
    };
    assert_eq!(
        get_intent("/dir", drop, &exists),
        asset("/dir/index.html", 200)
    );

    let fallback = IntentConfig {
        html_handling: HtmlHandling::None,
        not_found_handling: NotFoundHandling::NotFoundPage,
        ..auto
    };
    assert_eq!(
        get_intent("/missing/nested", fallback, &exists),
        asset("/404.html", 404)
    );
}

// Upstream compares only the `ETag`, so identical files under the two names redirect.
#[test]
fn safe_redirect_compares_only_the_etag() {
    let same = |path: &str| {
        ["/about.html", "/about/index.html"]
            .contains(&path)
            .then(String::new)
    };
    let redirect = |destination: &str| Some(Intent::Redirect(destination.to_owned()));
    let force = html_handling(HtmlHandling::ForceTrailingSlash);
    assert_eq!(get_intent("/about.html", force, &same), redirect("/about/"));
    let drop = html_handling(HtmlHandling::DropTrailingSlash);
    assert_eq!(
        get_intent("/about/index.html", drop, &same),
        redirect("/about")
    );
}

// Upstream: `handleRequest` answers a matching asset with 405 for anything but GET and HEAD,
// generated redirects included (base-path-handler.test.ts `returns 405 for POST at the base
// root` (L738) and `allows HEAD to use the base-root redirect` (L764)).
#[test]
fn only_get_and_head_reach_an_asset() {
    let exists = assets(&["/foo.html", "/index.html"]);
    let respond = |base_path, pathname, method| {
        respond_under(
            base_path,
            TestRequest {
                method,
                ..request(pathname)
            },
            IntentConfig::default(),
            &exists,
        )
    };
    assert_eq!(respond("/", "/foo", Method::Head), ASSET);
    assert_eq!(
        respond("/", "/foo", Method::Other),
        AssetResponse::MethodNotAllowed
    );
    assert_eq!(
        respond("/", "/missing", Method::Other),
        AssetResponse::NoIntent
    );
    assert_eq!(
        respond("/subpath", "/subpath", Method::Other),
        AssetResponse::MethodNotAllowed
    );
    assert_eq!(
        respond("/subpath", "/subpath", Method::Head),
        redirect("/subpath/")
    );
}

// Upstream: `handleRedirects` keeps 301, 303, 307 and 308 and answers any other status with
// 302. The location inherits the request's query when the rule has none.
#[test]
fn redirect_rule_status_and_location() {
    let exists = assets(&[]);
    for (status, expected) in [
        (301, 301),
        (302, 302),
        (303, 303),
        (307, 307),
        (308, 308),
        (300, 302),
        (418, 302),
    ] {
        let url = Url::parse("https://example.com/old?source=test").unwrap();
        let response = respond(
            &Request {
                url: &url,
                method: Method::Other,
                sec_fetch_mode: None,
                sec_fetch_dest: None,
            },
            Some(&Redirect {
                status,
                to: "/new".into(),
            }),
            IntentConfig::default(),
            &BasePath::default(),
            &exists,
        );
        assert_eq!(
            response,
            Ok(AssetResponse::Redirect {
                status: expected,
                location: "/new?source=test".into(),
            }),
            "{status}"
        );
    }
}

// A rewrite is looked up, and canonicalized, as its target.
#[test]
fn rewrite_resolves_its_target() {
    let exists = assets(&["/users/id.html"]);
    let rewrite = |to| {
        respond_under(
            "/",
            TestRequest {
                redirect: RedirectAction::Rewrite(to),
                ..request("/users/12345?tab=posts")
            },
            IntentConfig::default(),
            &exists,
        )
    };
    assert_eq!(rewrite("/users/id"), ASSET);
    assert_eq!(rewrite("/users/id.html"), redirect("/users/id?tab=posts"));
    assert_eq!(rewrite("/users/missing"), AssetResponse::NotFound);
}

fn redirect(location: &str) -> AssetResponse {
    AssetResponse::Redirect {
        status: 307,
        location: location.to_owned(),
    }
}

fn html_handling(html_handling: HtmlHandling) -> IntentConfig {
    IntentConfig {
        html_handling,
        ..IntentConfig::default()
    }
}

// Upstream: base-path-handler.test.ts `serves an in-prefix asset by looking up the stripped
// path` (L69), `returns NoIntent for an off-prefix request` (L173) and `is segment-aware:
// /subpath-other is off-prefix` (L191).
#[test]
fn base_path_is_stripped_before_asset_lookup() {
    let config = html_handling(HtmlHandling::None);
    let foo = assets(&["/foo"]);
    let everything = |path: &str| Some(path.to_owned());
    assert_eq!(
        respond_under("/subpath", request("/subpath/foo"), config, &foo),
        ASSET
    );
    assert_eq!(
        respond_under("/subpath", request("/foo"), config, &foo),
        AssetResponse::NoIntent
    );
    assert_eq!(
        respond_under("/subpath", request("/other"), config, &everything),
        AssetResponse::NoIntent
    );
    assert_eq!(
        respond_under(
            "/subpath",
            request("/subpath-other/foo"),
            config,
            &everything
        ),
        AssetResponse::NoIntent
    );
}

// Upstream: base-path-handler.test.ts `canonicalizes $publicPath once before prefix lookup`
// (L125).
#[test]
fn base_path_lookup_follows_path_canonicalization() {
    for (public_path, asset_path, location) in [
        ("/subpath//foo", "/foo", "/subpath/foo"),
        ("/subpath%2Ffoo", "/foo", "/subpath/foo"),
        ("/subpath/%ZZ", "/%ZZ", "/subpath/%25ZZ"),
        ("/subpath/%FF", "/%FF", "/subpath/%25FF"),
    ] {
        assert_eq!(
            respond_under(
                "/subpath",
                request(public_path),
                html_handling(HtmlHandling::None),
                &assets(&[asset_path]),
            ),
            redirect(location),
            "{public_path}"
        );
    }
}

// Upstream: base-path-handler.test.ts `re-prefixes a generated HTML redirect location` (L209)
// and `preserves the query string on a generated redirect` (L234).
#[test]
fn base_path_is_added_to_generated_redirects() {
    let exists = assets(&["/foo/index.html"]);
    assert_eq!(
        respond_under(
            "/subpath",
            request("/subpath/foo"),
            IntentConfig::default(),
            &exists
        ),
        redirect("/subpath/foo/")
    );
    assert_eq!(
        respond_under(
            "/subpath",
            request("/subpath/foo?a=1"),
            IntentConfig::default(),
            &exists
        ),
        redirect("/subpath/foo/?a=1")
    );
}

// Upstream: base-path-handler.test.ts `resolves an in-prefix SPA fallback to the
// asset-directory index` (L259).
#[test]
fn base_path_not_found_handling_resolves_in_the_asset_directory() {
    let config = IntentConfig {
        html_handling: HtmlHandling::None,
        not_found_handling: NotFoundHandling::SinglePageApplication,
        ..IntentConfig::default()
    };
    let exists = assets(&["/index.html"]);
    assert_eq!(
        respond_under("/subpath", request("/subpath/missing"), config, &exists),
        ASSET
    );
    assert_eq!(
        respond_under("/subpath", request("/missing"), config, &exists),
        AssetResponse::NoIntent
    );
}

// Upstream: base-path-handler.test.ts `accepts an authored 200 rewrite with an in-prefix
// public target` (L286), `claims an authored 200 rewrite whose target is off-prefix` (L320),
// `ignores an authored 200 rewrite from an off-prefix source` (L365), `ignores an off-prefix
// authored non-200 redirect` (L399) and `gives an authored redirect precedence at the base
// root` (L640).
#[test]
fn base_path_applies_to_the_source_of_a_redirect_rule() {
    let config = html_handling(HtmlHandling::ForceTrailingSlash);
    let exists = assets(&["/foo", "/index.html"]);
    let authored = AssetResponse::Redirect {
        status: 301,
        location: "/redirected".into(),
    };
    for (pathname, redirect, expected) in [
        (
            "/subpath/example",
            RedirectAction::Rewrite("/subpath/foo"),
            ASSET,
        ),
        (
            "/subpath/example",
            RedirectAction::Rewrite("/foo"),
            AssetResponse::NotFound,
        ),
        (
            "/other",
            RedirectAction::Rewrite("/subpath/foo"),
            AssetResponse::NoIntent,
        ),
        ("/other", RedirectAction::Redirect, AssetResponse::NoIntent),
        (
            "/subpath/example",
            RedirectAction::Redirect,
            authored.clone(),
        ),
        ("/subpath", RedirectAction::Redirect, authored),
    ] {
        assert_eq!(
            respond_under(
                "/subpath",
                TestRequest {
                    redirect,
                    ..request(pathname)
                },
                config,
                &exists
            ),
            expected,
            "{pathname} {redirect:?}"
        );
    }
}

// Upstream: base-path-handler.test.ts `redirects the base root to a trailing slash under
// force-trailing-slash` (L540), `redirects the base root under auto-trailing-slash and
// preserves the query` (L566), `redirects the slash base root under drop-trailing-slash`
// (L591) and `does not generate a base-root redirect under html_handling none` (L616).
#[test]
fn base_root_redirects_to_its_canonical_spelling() {
    let exists = assets(&["/index.html"]);
    for (handling, pathname, expected) in [
        (
            HtmlHandling::ForceTrailingSlash,
            "/subpath",
            redirect("/subpath/"),
        ),
        (HtmlHandling::ForceTrailingSlash, "/subpath/", ASSET),
        (
            HtmlHandling::AutoTrailingSlash,
            "/subpath?q=1",
            redirect("/subpath/?q=1"),
        ),
        (HtmlHandling::AutoTrailingSlash, "/subpath/", ASSET),
        (
            HtmlHandling::DropTrailingSlash,
            "/subpath/?q=1",
            redirect("/subpath?q=1"),
        ),
        (HtmlHandling::DropTrailingSlash, "/subpath", ASSET),
        (HtmlHandling::None, "/subpath", AssetResponse::NoIntent),
        (HtmlHandling::None, "/subpath/", AssetResponse::NoIntent),
    ] {
        assert_eq!(
            respond_under(
                "/subpath",
                request(pathname),
                html_handling(handling),
                &exists
            ),
            expected,
            "{handling:?} {pathname}"
        );
    }
}

// Upstream: base-path-handler.test.ts `does not redirect a base root without an index asset`
// (L669) and `does not generate a slash redirect for a non-index root asset` (L690).
#[test]
fn base_root_redirect_requires_an_index() {
    let config = html_handling(HtmlHandling::ForceTrailingSlash);
    assert_eq!(
        respond_under("/subpath", request("/subpath"), config, &assets(&[])),
        AssetResponse::NoIntent
    );
    assert_eq!(
        respond_under("/subpath", request("/subpath"), config, &assets(&["/"])),
        ASSET
    );
}

// Upstream: base-path-handler.test.ts `percent-encodes a Unicode base-root redirect` (L711).
#[test]
fn base_root_redirect_is_percent_encoded() {
    assert_eq!(
        respond_under(
            "/路径",
            request("/%E8%B7%AF%E5%BE%84"),
            html_handling(HtmlHandling::ForceTrailingSlash),
            &assets(&["/index.html"]),
        ),
        redirect("/%E8%B7%AF%E5%BE%84/")
    );
}

// A root base has a single spelling, so it never takes the base-root redirect.
#[test]
fn root_base_path_has_no_base_root_redirect() {
    let exists = assets(&["/index.html"]);
    for handling in [
        HtmlHandling::AutoTrailingSlash,
        HtmlHandling::ForceTrailingSlash,
        HtmlHandling::DropTrailingSlash,
    ] {
        assert_eq!(
            respond_under("/", request("/"), html_handling(handling), &exists),
            ASSET,
            "{handling:?}"
        );
    }
}

#[test]
fn decode_path_keeps_segments_decode_uri_component_throws_on() {
    assert_eq!(decode_path("/hello%20world/%2E%2E"), "/hello world/..");
    for segment in ["%", "%A", "%ZZ", "%+1", "%A0", "%A0%A0"] {
        assert_eq!(decode_path(&format!("/{segment}")), format!("/{segment}"));
    }
}

// Upstream: handler.test.ts `cannot fetch assets outside of configured path` (L133).
//
// The handler does not resolve `..`; it decodes, and the host refuses the path. Both the raw `..`
// and the percent-encoded `%2E%2E` forms decode to the same path, so neither escapes the
// directory.
#[test]
fn decode_path_blocks_traversal() {
    assert_eq!(decode_path("/blog/../test"), "/blog/../test");
}

#[test]
fn decode_path_blocks_percent_encoded_traversal() {
    // `%2E%2E` → `..`, identical to the raw form above.
    assert_eq!(decode_path("/blog/%2E%2E/test"), "/blog/../test");
}

// Upstream: handler.test.ts `returns expected responses for malformed path` (L190).
//
// `%A0` is a valid percent escape but not valid UTF-8, so `decodeURIComponent` throws and the
// segment is kept verbatim. This lets `/%A0%A0` still match an asset `/%A0%A0.html`.
#[test]
fn decode_path_preserves_non_utf8_percent_sequences() {
    assert_eq!(decode_path("/%A0"), "/%A0");
    assert_eq!(decode_path("/%A0%A0"), "/%A0%A0");
}

#[test]
fn decode_path_collapses_multiple_slashes() {
    assert_eq!(decode_path("/foo//bar///baz"), "/foo/bar/baz");
    assert_eq!(decode_path("//"), "/");
}

#[test]
fn encode_path_encodes_reserved_but_not_slashes() {
    // Brackets are reserved and get encoded; slashes are the segment separator.
    assert_eq!(encode_path("/[boop]"), "/%5Bboop%5D");
    // Unreserved chars pass through.
    assert_eq!(encode_path("/foo-bar_baz.html"), "/foo-bar_baz.html");
    // Spaces encode to %20 (not +).
    assert_eq!(encode_path("/a b"), "/a%20b");
}

#[test]
fn encode_path_round_trips_unreserved_marks() {
    // encodeURIComponent leaves - _ . ! ~ * ' ( ) untouched.
    assert_eq!(encode_path("/-_.!~*'()"), "/-_.!~*'()");
}
