use super::super::header;
use super::*;

fn values(matcher: &RulesMatcher<u8>, hostname: &str, pathname: &str) -> Vec<u8> {
    matcher
        .matches(hostname, pathname)
        .map(|(value, _)| *value)
        .collect()
}

fn rendered(rules: &RulesMatcher<&str>, hostname: &str, pathname: &str) -> Vec<String> {
    let found = rules.matches(hostname, pathname);
    found
        .map(|(value, captures)| replacer(value, &captures))
        .collect()
}

fn url(pathname: &str) -> Url {
    Url::parse(&format!("https://example.com{pathname}")).unwrap()
}

/// The target of the rule that applies to a request for `pathname`.
fn found(redirects: &Redirects, pathname: &str) -> String {
    redirects.find(&url(pathname)).unwrap().to
}

#[test]
fn rules_matcher_matches_simple_pathnames() {
    let matcher = RulesMatcher::new([("/test".into(), 1), ("/some%20page".into(), 2)]);
    assert_eq!(values(&matcher, "example.com", "/test"), [1]);
    assert_eq!(values(&matcher, "example.com", "/some%20page"), [2]);
}

#[test]
fn rules_matcher_matches_cross_host_requests() {
    let matcher = RulesMatcher::new([
        ("/test".into(), 1),
        ("/anotherpage".into(), 2),
        ("https://custom.domain/test".into(), 3),
        ("https://another.domain/test".into(), 4),
        ("//fake.host/actually-a-path".into(), 5),
    ]);
    assert_eq!(values(&matcher, "custom.domain", "/test"), [1, 3]);
    assert_eq!(
        values(&matcher, "example.com", "//fake.host/actually-a-path"),
        [5]
    );
    assert_eq!(values(&matcher, "custom.domain", "/test"), [1, 3]);
}

#[test]
fn rules_matcher_escapes_regex_metacharacters() {
    let matcher = RulesMatcher::new([("/$~.%20/!+-/[bo|%7Bo%7D]...()".into(), 1)]);
    assert_eq!(
        values(&matcher, "example.com", "/$~.%20/!+-/[bo|%7Bo%7D]...()"),
        [1]
    );
    assert_eq!(values(&matcher, "example.com", "/anything-else"), [0u8; 0]);
}

#[test]
fn rules_matcher_supports_splats_and_placeholders() {
    let matcher = RulesMatcher::new([
        ("/foo/test/*".into(), "1/:splat"),
        ("/foo/*".into(), "2/:splat"),
        ("/blog/:code/:name".into(), "3/:name/:code"),
        ("/blog/*".into(), "4/:splat"),
        (
            "https://:subdomain.pages.:tld/*".into(),
            "5/:subdomain/:tld/:splat",
        ),
        (
            "https://next.:subdomain.pages.:tld/*".into(),
            "6/:subdomain/",
        ),
    ]);
    let rendered = |host, path| rendered(&matcher, host, path);
    assert_eq!(
        rendered("example.com", "/foo/test/yes"),
        ["1/yes", "2/test/yes"]
    );
    assert_eq!(rendered("example.com", "/foo/test/"), ["1/", "2/test/"]);
    assert_eq!(rendered("example.com", "/foo/test"), ["2/test"]);
    assert_eq!(rendered("example.com", "/foo/"), ["2/"]);
    assert!(rendered("example.com", "/foo").is_empty());
    assert_eq!(
        rendered("example.com", "/blog/123/tricycle"),
        ["3/tricycle/123", "4/123/tricycle"]
    );
    assert_eq!(rendered("my.pages.dev", "/magic"), ["5/my/dev/magic"]);
    assert_eq!(rendered("next.my.pages.dev", "/magic"), ["6/my/"]);
}

#[test]
fn replacer_replaces_splats() {
    assert_eq!(
        replacer("/blog/:splat", &[("splat".into(), "look/a/value".into())]),
        "/blog/look/a/value"
    );
}

#[test]
fn replacer_replaces_placeholders() {
    assert_eq!(
        replacer(
            "/:code/:name.jpg",
            &[
                ("name".into(), "tricycle".into()),
                ("code".into(), "123".into())
            ]
        ),
        "/123/tricycle.jpg"
    );
}

#[test]
fn replacer_replaces_splats_and_placeholders() {
    assert_eq!(
        replacer(
            "/:code/:splat",
            &[
                ("splat".into(), "tricycle/images".into()),
                ("code".into(), "123".into())
            ]
        ),
        "/123/tricycle/images"
    );
}

#[test]
fn replacer_replaces_all_instances_globally() {
    let input = "Link: </assets/:value/main.js>; rel=preload; as=script, </assets/:value/lang.js>; rel=preload; as=script";
    assert_eq!(
        replacer(input, &[("value".into(), "js".into())]),
        "Link: </assets/js/main.js>; rel=preload; as=script, </assets/js/lang.js>; rel=preload; as=script"
    );
}

#[test]
fn replacer_preserves_capture_order_for_prefix_names() {
    let matcher = RulesMatcher::new([("/:foobar/:foo".into(), "/:foobar/:foo")]);
    assert_eq!(
        rendered(&matcher, "example.com", "/long/short"),
        ["/long/short"]
    );
}

#[test]
fn redirect_location_matches_url_resolution_and_query_inheritance() {
    let request = url::Url::parse("https://example.com/a/old?orig=1").unwrap();
    assert_eq!(resolve_location(&request, "new").unwrap(), "/a/new?orig=1");
    assert_eq!(resolve_location(&request, "/new").unwrap(), "/new?orig=1");
    assert_eq!(resolve_location(&request, "/new?x=2").unwrap(), "/new?x=2");
    assert_eq!(resolve_location(&request, "/new?").unwrap(), "/new?orig=1");
    assert_eq!(
        resolve_location(&request, "/new#frag").unwrap(),
        "/new?orig=1#frag"
    );
    assert_eq!(resolve_location(&request, "/new#").unwrap(), "/new?orig=1");
    assert_eq!(
        resolve_location(&request, "https://other.test/new").unwrap(),
        "https://other.test/new?orig=1"
    );
}

#[test]
fn redirects_static_and_dynamic_scenarios() {
    let static_rules = [
        (
            "/compete".into(),
            (
                4,
                Redirect {
                    status: 302,
                    to: "/hostless".into(),
                },
            ),
        ),
        (
            "https://example.com/compete".into(),
            (
                5,
                Redirect {
                    status: 302,
                    to: "/withhost".into(),
                },
            ),
        ),
        (
            "https://example.com/backwards".into(),
            (
                6,
                Redirect {
                    status: 302,
                    to: "/withhost".into(),
                },
            ),
        ),
        (
            "/backwards".into(),
            (
                7,
                Redirect {
                    status: 302,
                    to: "/hostless".into(),
                },
            ),
        ),
    ];
    let redirects = Redirects::new(static_rules, []);
    assert_eq!(found(&redirects, "/compete"), "/hostless");
    assert_eq!(found(&redirects, "/backwards"), "/withhost");

    let redirects = Redirects::new(
        [],
        [
            (
                "/dynamic/:seg".into(),
                Redirect {
                    status: 302,
                    to: "/:seg/new-dynamic/?with#params".into(),
                },
            ),
            (
                "/splat/*".into(),
                Redirect {
                    status: 302,
                    to: "/:splat/new-splat".into(),
                },
            ),
            (
                "/splat/foo/*".into(),
                Redirect {
                    status: 302,
                    to: "/will-never-fire".into(),
                },
            ),
            (
                "/partialPlaceholder:placeholder".into(),
                Redirect {
                    status: 302,
                    to: "/new-partialPlaceholder:placeholder".into(),
                },
            ),
        ],
    );
    assert_eq!(
        found(&redirects, "/dynamic/foo"),
        "/foo/new-dynamic/?with#params"
    );
    assert_eq!(found(&redirects, "/splat/foo/bar"), "/foo/bar/new-splat");
    assert_eq!(
        found(&redirects, "/partialPlaceholderfoo"),
        "/new-partialPlaceholderfoo"
    );
    assert_eq!(redirects.find(&url("/partialPlaceholderfoo/")), None);
}

#[test]
fn static_rule_takes_precedence_and_matches_a_host_with_a_port() {
    let rule = |to: &str, line: usize| {
        (
            line,
            Redirect {
                status: 301,
                to: to.into(),
            },
        )
    };
    let redirects = Redirects::new(
        [
            ("/old".into(), rule("/static", 1)),
            ("https://example.com:8443/ported".into(), rule("/port", 2)),
        ],
        [(
            "/*".into(),
            Redirect {
                status: 302,
                to: "/dynamic".into(),
            },
        )],
    );
    assert_eq!(found(&redirects, "/old"), "/static");
    assert_eq!(found(&redirects, "/other"), "/dynamic");
    assert_eq!(found(&redirects, "/ported"), "/dynamic");
    assert_eq!(
        redirects
            .find(&Url::parse("https://example.com:8443/ported").unwrap())
            .unwrap()
            .to,
        "/port"
    );
}

#[test]
fn open_redirect_defense_normalizes_double_slash() {
    let redirects = Redirects::new(
        [],
        [(
            "/foo/*".into(),
            Redirect {
                status: 302,
                to: "/:splat".into(),
            },
        )],
    );
    assert_eq!(found(&redirects, "/foo//google.com"), "/google.com");
}

#[test]
fn applies_matching_rules_in_order() {
    let matcher = RulesMatcher::new([
        (
            "/blog/:slug".into(),
            HeaderRule {
                set: vec![("Location".into(), "/first/:slug".into())],
                unset: vec!["X-Remove".into()],
            },
        ),
        (
            "/blog/*".into(),
            HeaderRule {
                set: vec![
                    ("location".into(), "/second/:splat".into()),
                    ("X-Added".into(), ":splat".into()),
                ],
                unset: vec![],
            },
        ),
    ]);

    assert_eq!(
        apply_headers(
            &matcher,
            "example.com",
            "/blog/post",
            vec![header("Location", "/original"), header("X-Remove", "yes")],
        ),
        [
            header("Location", "/first/post"),
            header("location", "/second/post"),
            header("X-Added", "post"),
        ]
    );
}

// Upstream `handleRequest > _headers` unsets a header whose case differs from the response
// (`unset: ["ETAg"]` against an `ETag` header). Unset must be ASCII-case-insensitive.
#[test]
fn unset_is_case_insensitive() {
    let matcher = RulesMatcher::new([(
        "/art/*".into(),
        HeaderRule {
            set: vec![],
            unset: vec!["ETAg".into()],
        },
    )]);
    assert_eq!(
        apply_headers(
            &matcher,
            "example.com",
            "/art/x",
            vec![header("ETag", "\"abc\""), header("X-Keep", "1")],
        ),
        [header("X-Keep", "1")]
    );
}
