// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Golden parity vectors for the highest-risk pure functions (path encode/decode, the
//! `_redirects`/static-routing rules engine, and the placeholder replacer).
//!
//! Recorded from the output of the *verbatim* upstream workers-sdk
//! functions (`asset-worker/src/handler.ts` decodePath/encodePath and
//! `asset-worker/src/utils/rules-engine.ts`), run over a fixed input set at snapshot main @
//! 2026-07-14 (commit 2cd84d4). Those functions, and `generateStaticRoutingRuleMatcher`, are
//! unchanged from there to dc47e6d2, the commit this crate is ported from. The generator is not
//! in this tree: to extend the vectors, run the upstream functions over new inputs and record
//! their outputs here. These assert the native Rust ports reproduce the upstream
//! JS output byte-for-byte, catching divergences the hand-picked ported unit tests might miss.

use crate::decide::globs;
use crate::intent::decode_path;
use crate::intent::encode_path;
use crate::rules::RulesMatcher;
use crate::rules::replacer;

/// (input, upstream decodePath, upstream encodePath)
const PATH_VECTORS: &[(&str, &str, &str)] = &[
    ("/", "/", "/"),
    ("/foo", "/foo", "/foo"),
    ("/foo/bar", "/foo/bar", "/foo/bar"),
    ("/foo//bar", "/foo/bar", "/foo//bar"),
    ("///a///b///", "/a/b/", "///a///b///"),
    ("/foo.html", "/foo.html", "/foo.html"),
    ("/dir/", "/dir/", "/dir/"),
    ("/[boop].html", "/[boop].html", "/%5Bboop%5D.html"),
    ("/%5Bboop%5D.html", "/[boop].html", "/%255Bboop%255D.html"),
    ("/%255Bboop%255D", "/%5Bboop%5D", "/%25255Bboop%25255D"),
    ("/caf\u{e9}", "/caf\u{e9}", "/caf%C3%A9"),
    ("/caf%C3%A9", "/caf\u{e9}", "/caf%25C3%25A9"),
    ("/a b", "/a b", "/a%20b"),
    ("/a%20b", "/a b", "/a%2520b"),
    ("/a+b", "/a+b", "/a%2Bb"),
    ("/%zz", "/%zz", "/%25zz"),
    ("/%", "/%", "/%25"),
    ("/%2", "/%2", "/%252"),
    ("/100%", "/100%", "/100%25"),
    ("/%E0%A4%A", "/%E0%A4%A", "/%25E0%25A4%25A"),
    (
        "/hello%20world/%2E%2E",
        "/hello world/..",
        "/hello%2520world/%252E%252E",
    ),
    ("/../etc/passwd", "/../etc/passwd", "/../etc/passwd"),
    ("/%2e%2e/x", "/../x", "/%252e%252e/x"),
    (
        "/emoji/\u{1f600}",
        "/emoji/\u{1f600}",
        "/emoji/%F0%9F%98%80",
    ),
    ("/space test/path", "/space test/path", "/space%20test/path"),
    ("/a?b#c", "/a?b#c", "/a%3Fb%23c"),
    ("/\u{a0}", "/\u{a0}", "/%C2%A0"),
    ("/%A0", "/%A0", "/%25A0"),
    (
        "/tilde~/dash-/star*",
        "/tilde~/dash-/star*",
        "/tilde~/dash-/star*",
    ),
    ("/(paren)", "/(paren)", "/(paren)"),
    ("/a=b&c=d", "/a=b&c=d", "/a%3Db%26c%3Dd"),
    ("/path%2Fslash", "/path/slash", "/path%252Fslash"),
    (
        "/mixed/CaSe/File.HTML",
        "/mixed/CaSe/File.HTML",
        "/mixed/CaSe/File.HTML",
    ),
    ("/trailing%2f", "/trailing/", "/trailing%252f"),
    ("/%c3%28", "/%c3%28", "/%25c3%2528"),
    (
        "/\u{221a}/\u{3c0}",
        "/\u{221a}/\u{3c0}",
        "/%E2%88%9A/%CF%80",
    ),
    ("/;semi", "/;semi", "/%3Bsemi"),
    ("/@at", "/@at", "/%40at"),
];

#[test]
fn golden_path_encode_decode() {
    for (input, decoded, encoded) in PATH_VECTORS {
        assert_eq!(&decode_path(input), decoded, "decode_path({input:?})");
        assert_eq!(&encode_path(input), encoded, "encode_path({input:?})");
    }
}

/// (rules, pathname, upstream generateStaticRoutingRuleMatcher result)
const STATIC_ROUTING_VECTORS: &[(&[&str], &str, bool)] = &[
    (&["/"], "/", true),
    (&["/"], "/foo", false),
    (&["/"], "/foo/", false),
    (&["/"], "/foobar", false),
    (&["/"], "/foo/bar", false),
    (&["/"], "/:placeholder", false),
    (&["/"], "/anything", false),
    (&["/"], "/api/", false),
    (&["/"], "/api/x/y", false),
    (&["/"], "/a.b", false),
    (&["/"], "/axb", false),
    (&["/"], "/x+y", false),
    (&["/"], "/xy", false),
    (&["/"], "/deep/mid/end", false),
    (&["/"], "/deep/a/b/end", false),
    (&["/"], "/exact", false),
    (&["/"], "/exacts", false),
    (&["/foo"], "/", false),
    (&["/foo"], "/foo", true),
    (&["/foo"], "/foo/", false),
    (&["/foo"], "/foobar", false),
    (&["/foo"], "/foo/bar", false),
    (&["/foo"], "/:placeholder", false),
    (&["/foo"], "/anything", false),
    (&["/foo"], "/api/", false),
    (&["/foo"], "/api/x/y", false),
    (&["/foo"], "/a.b", false),
    (&["/foo"], "/axb", false),
    (&["/foo"], "/x+y", false),
    (&["/foo"], "/xy", false),
    (&["/foo"], "/deep/mid/end", false),
    (&["/foo"], "/deep/a/b/end", false),
    (&["/foo"], "/exact", false),
    (&["/foo"], "/exacts", false),
    (&["/foo*"], "/", false),
    (&["/foo*"], "/foo", true),
    (&["/foo*"], "/foo/", true),
    (&["/foo*"], "/foobar", true),
    (&["/foo*"], "/foo/bar", true),
    (&["/foo*"], "/:placeholder", false),
    (&["/foo*"], "/anything", false),
    (&["/foo*"], "/api/", false),
    (&["/foo*"], "/api/x/y", false),
    (&["/foo*"], "/a.b", false),
    (&["/foo*"], "/axb", false),
    (&["/foo*"], "/x+y", false),
    (&["/foo*"], "/xy", false),
    (&["/foo*"], "/deep/mid/end", false),
    (&["/foo*"], "/deep/a/b/end", false),
    (&["/foo*"], "/exact", false),
    (&["/foo*"], "/exacts", false),
    (&["/:placeholder"], "/", false),
    (&["/:placeholder"], "/foo", false),
    (&["/:placeholder"], "/foo/", false),
    (&["/:placeholder"], "/foobar", false),
    (&["/:placeholder"], "/foo/bar", false),
    (&["/:placeholder"], "/:placeholder", true),
    (&["/:placeholder"], "/anything", false),
    (&["/:placeholder"], "/api/", false),
    (&["/:placeholder"], "/api/x/y", false),
    (&["/:placeholder"], "/a.b", false),
    (&["/:placeholder"], "/axb", false),
    (&["/:placeholder"], "/x+y", false),
    (&["/:placeholder"], "/xy", false),
    (&["/:placeholder"], "/deep/mid/end", false),
    (&["/:placeholder"], "/deep/a/b/end", false),
    (&["/:placeholder"], "/exact", false),
    (&["/:placeholder"], "/exacts", false),
    (&["/api/*"], "/", false),
    (&["/api/*"], "/foo", false),
    (&["/api/*"], "/foo/", false),
    (&["/api/*"], "/foobar", false),
    (&["/api/*"], "/foo/bar", false),
    (&["/api/*"], "/:placeholder", false),
    (&["/api/*"], "/anything", false),
    (&["/api/*"], "/api/", true),
    (&["/api/*"], "/api/x/y", true),
    (&["/api/*"], "/a.b", false),
    (&["/api/*"], "/axb", false),
    (&["/api/*"], "/x+y", false),
    (&["/api/*"], "/xy", false),
    (&["/api/*"], "/deep/mid/end", false),
    (&["/api/*"], "/deep/a/b/end", false),
    (&["/api/*"], "/exact", false),
    (&["/api/*"], "/exacts", false),
    (&["/a.b"], "/", false),
    (&["/a.b"], "/foo", false),
    (&["/a.b"], "/foo/", false),
    (&["/a.b"], "/foobar", false),
    (&["/a.b"], "/foo/bar", false),
    (&["/a.b"], "/:placeholder", false),
    (&["/a.b"], "/anything", false),
    (&["/a.b"], "/api/", false),
    (&["/a.b"], "/api/x/y", false),
    (&["/a.b"], "/a.b", true),
    (&["/a.b"], "/axb", false),
    (&["/a.b"], "/x+y", false),
    (&["/a.b"], "/xy", false),
    (&["/a.b"], "/deep/mid/end", false),
    (&["/a.b"], "/deep/a/b/end", false),
    (&["/a.b"], "/exact", false),
    (&["/a.b"], "/exacts", false),
    (&["/x+y"], "/", false),
    (&["/x+y"], "/foo", false),
    (&["/x+y"], "/foo/", false),
    (&["/x+y"], "/foobar", false),
    (&["/x+y"], "/foo/bar", false),
    (&["/x+y"], "/:placeholder", false),
    (&["/x+y"], "/anything", false),
    (&["/x+y"], "/api/", false),
    (&["/x+y"], "/api/x/y", false),
    (&["/x+y"], "/a.b", false),
    (&["/x+y"], "/axb", false),
    (&["/x+y"], "/x+y", true),
    (&["/x+y"], "/xy", false),
    (&["/x+y"], "/deep/mid/end", false),
    (&["/x+y"], "/deep/a/b/end", false),
    (&["/x+y"], "/exact", false),
    (&["/x+y"], "/exacts", false),
    (&["/deep/*/end"], "/", false),
    (&["/deep/*/end"], "/foo", false),
    (&["/deep/*/end"], "/foo/", false),
    (&["/deep/*/end"], "/foobar", false),
    (&["/deep/*/end"], "/foo/bar", false),
    (&["/deep/*/end"], "/:placeholder", false),
    (&["/deep/*/end"], "/anything", false),
    (&["/deep/*/end"], "/api/", false),
    (&["/deep/*/end"], "/api/x/y", false),
    (&["/deep/*/end"], "/a.b", false),
    (&["/deep/*/end"], "/axb", false),
    (&["/deep/*/end"], "/x+y", false),
    (&["/deep/*/end"], "/xy", false),
    (&["/deep/*/end"], "/deep/mid/end", true),
    (&["/deep/*/end"], "/deep/a/b/end", true),
    (&["/deep/*/end"], "/exact", false),
    (&["/deep/*/end"], "/exacts", false),
    (&["/exact"], "/", false),
    (&["/exact"], "/foo", false),
    (&["/exact"], "/foo/", false),
    (&["/exact"], "/foobar", false),
    (&["/exact"], "/foo/bar", false),
    (&["/exact"], "/:placeholder", false),
    (&["/exact"], "/anything", false),
    (&["/exact"], "/api/", false),
    (&["/exact"], "/api/x/y", false),
    (&["/exact"], "/a.b", false),
    (&["/exact"], "/axb", false),
    (&["/exact"], "/x+y", false),
    (&["/exact"], "/xy", false),
    (&["/exact"], "/deep/mid/end", false),
    (&["/exact"], "/deep/a/b/end", false),
    (&["/exact"], "/exact", true),
    (&["/exact"], "/exacts", false),
    (&["/foo", "/bar*"], "/barbaz", true),
    (&["/foo", "/bar*"], "/nope", false),
];

#[test]
fn golden_static_routing() {
    for (rules, pathname, expected) in STATIC_ROUTING_VECTORS {
        let owned: Vec<String> = rules.iter().map(|r| (*r).to_owned()).collect();
        let matcher = globs(&owned);
        assert_eq!(
            matcher.iter().any(|r| r.is_match(pathname)),
            *expected,
            "rules={rules:?} path={pathname:?}"
        );
    }
}

/// (rules as (pattern, target) pairs, host, pathname, upstream matched+replaced targets)
type RulesVector = (
    &'static [(&'static str, &'static str)],
    &'static str,
    &'static str,
    &'static [&'static str],
);
const RULES_VECTORS: &[RulesVector] = &[
    (&[("/old", "/new")], "example.com", "/old", &["/new"]),
    (
        &[("/blog/*", "/posts/:splat")],
        "example.com",
        "/blog/a/b",
        &["/posts/a/b"],
    ),
    (
        &[("/users/:id", "/u/:id")],
        "example.com",
        "/users/42",
        &["/u/42"],
    ),
    (&[("/:a/:b", "/:b/:a")], "example.com", "/x/y", &["/y/x"]),
    (
        &[("https://:sub.example.com/", "/tenant/:sub")],
        "acme.example.com",
        "/",
        &["/tenant/acme"],
    ),
    (
        &[("/multi/:x", "/:x/:x/:x")],
        "example.com",
        "/multi/z",
        &["/z/z/z"],
    ),
    (&[("/a", "/1"), ("/b", "/2")], "example.com", "/b", &["/2"]),
    (&[("/none", "/x")], "example.com", "/other", &[]),
    (
        &[("/sp/*", "/dst/:splat/end")],
        "example.com",
        "/sp/deep/path",
        &["/dst/deep/path/end"],
    ),
];

#[test]
fn golden_rules_engine() {
    for (rules, host, pathname, expected) in RULES_VECTORS {
        let matcher = RulesMatcher::new(
            rules
                .iter()
                .map(|(pattern, target)| ((*pattern).to_owned(), (*target).to_owned())),
        );
        let got: Vec<String> = matcher
            .matches(host, pathname)
            .map(|(target, repl)| replacer(target, &repl))
            .collect();
        let expected: Vec<String> = expected.iter().map(|s| (*s).to_owned()).collect();
        assert_eq!(
            got, expected,
            "rules={rules:?} host={host:?} path={pathname:?}"
        );
    }
}

/// (template, replacements as (name, value) pairs, upstream replacer output)
type ReplacerVector = (
    &'static str,
    &'static [(&'static str, &'static str)],
    &'static str,
);
const REPLACER_VECTORS: &[ReplacerVector] = &[
    ("/:a/:b", &[("a", "1"), ("b", "2")], "/1/2"),
    (
        "/static/:value/main.:value",
        &[("value", "js")],
        "/static/js/main.js",
    ),
    ("/no/placeholders", &[("x", "y")], "/no/placeholders"),
    ("/:missing", &[], "/:missing"),
    (":a:b", &[("a", "X"), ("b", "Y")], "XY"),
];

#[test]
fn golden_replacer() {
    for (tpl, repl, expected) in REPLACER_VECTORS {
        let owned: Vec<(String, String)> = repl
            .iter()
            .map(|(n, v)| ((*n).to_owned(), (*v).to_owned()))
            .collect();
        assert_eq!(
            &replacer(tpl, &owned),
            expected,
            "replacer({tpl:?}, {repl:?})"
        );
    }
}
