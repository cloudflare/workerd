
KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?otherquery#otherhash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),.baseUrl = kj::str("https://example.com?query#hash"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/([^\\/]+?)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/:bar*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/*+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/**"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/**"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/**"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/**"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/**"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/(.*)*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/**"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo{/bar}*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str(":café"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.username = kj::str(":café"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.password = kj::str(":café"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str(":café"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/:café"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.search = kj::str(":café"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hash = kj::str(":café"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str(":℘"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.username = kj::str(":℘"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.password = kj::str(":℘"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str(":℘"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/:℘"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.search = kj::str(":℘"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hash = kj::str(":℘"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str(":㐀"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.username = kj::str(":㐀"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.password = kj::str(":㐀"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str(":㐀"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/:㐀"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.search = kj::str(":㐀"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hash = kj::str(":㐀"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("foo-bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.username = kj::str("caf%C3%A9"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.username = kj::str("café"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.username = kj::str("caf%c3%a9"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.password = kj::str("caf%C3%A9"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.password = kj::str("café"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.password = kj::str("caf%c3%a9"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("xn--caf-dma.com"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("café.com"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {

})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("http"),.port = kj::str("80"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("http"),.port = kj::str("80{20}?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.port = kj::str("80"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("http{s}?"),.port = kj::str("80"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.port = kj::str("80"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.port = kj::str("(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/baz"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/caf%C3%A9"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/café"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/caf%c3%a9"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/../bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("./foo/bar"),.baseUrl = kj::str("https://example.com"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.baseUrl = kj::str("https://example.com"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{/bar}"),.baseUrl = kj::str("https://example.com/foo/"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("\\/bar"),.baseUrl = kj::str("https://example.com/foo/"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("b"),.baseUrl = kj::str("https://example.com/foo/"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("foo/bar"),.baseUrl = kj::str("https://example.com"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str(":name.html"),.baseUrl = kj::str("https://example.com"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.search = kj::str("q=caf%C3%A9"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.search = kj::str("q=café"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.search = kj::str("q=caf%c3%a9"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hash = kj::str("caf%C3%A9"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hash = kj::str("café"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hash = kj::str("caf%c3%a9"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("about"),.pathname = kj::str("(blank|sourcedoc)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("data"),.pathname = kj::str(":number([0-9]+)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo!"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo\\:"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo\\{"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo\\("),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("javascript"),.pathname = kj::str("var x = 1;"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("var x = 1;"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("javascript"),.pathname = kj::str("var x = 1;"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("(data|javascript)"),.pathname = kj::str("var x = 1;"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("(https|javascript)"),.pathname = kj::str("var x = 1;"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("var x = 1;"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com:8080/foo?bar#baz"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/foo?bar#baz"_kj, UrlPattern::CompileOptions {.baseUrl = "https://example.com:8080"_kj})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("http{s}?://{*.}?example.com/:product/:endpoint"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com?foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com#foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com:8080?foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com:8080#foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com/?foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com/#foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com/*?foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com/*\?foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com/:name?foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com/:name\?foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com/(bar)?foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com/(bar)\?foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com/{bar}?foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com/{bar}\?foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com/"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("data\\:foobar"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://{sub.}?example.com/foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://(sub.)?example.com/foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://(sub.)?example(.com/)foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://(sub(?:.))?example.com/foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("file:///foo/bar"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("data:"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("foo://bar"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com/foo?bar#baz"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("?bar#baz"_kj, UrlPattern::CompileOptions {.baseUrl = "https://example.com/foo"_kj})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("?bar"_kj, UrlPattern::CompileOptions {.baseUrl = "https://example.com/foo#baz"_kj})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("#baz"_kj, UrlPattern::CompileOptions {.baseUrl = "https://example.com/foo?bar"_kj})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("#baz"_kj, UrlPattern::CompileOptions {.baseUrl = "https://example.com/foo"_kj})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://foo\\:bar@example.com"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://foo@example.com"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://\\:bar@example.com"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://:user::pass@example.com"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https\\:foo\\:bar@example.com"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("data\\:foo\\:bar@example.com"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://foo{\\:}bar@example.com"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("data{\\:}channel.html"_kj, UrlPattern::CompileOptions {.baseUrl = "https://example.com"_kj})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("http://[\\:\\:1]/"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("http://[\\:\\:1]:8080/"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("http://[\\:\\:a]/"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("http://[:address]/"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("http://[\\:\\:AB\\::num]/"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("[\\:\\:AB\\::num]"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("{[\\:\\:ab\\::num]}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("{[\\:\\::num\\:1]}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("[*\\:1]"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("data\\:text/javascript,let x = 100/:tens?5;"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str(":name*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str(":name+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str(":name"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str(":name*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str(":name+"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str(":name"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("(foo)(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{(foo)bar}(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("(foo)?(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{:foo}(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{:foo}(barbaz)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{:foo}{(.*)}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{:foo}{(.*)bar}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{:foo}{bar(.*)}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{:foo}:bar(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{:foo}?(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{:foo\bar}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{:foo\\.bar}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{:foo(foo)bar}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("{:foo}bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str(":foo\bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str(":foo{}(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str(":foo{}bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str(":foo{}?bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("*{}**?"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str(":foo(baz)(.*)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str(":foo(baz)bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("*/*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("*\\/*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("*/{*}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("*//*"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/:foo."),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/:foo.."),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("./foo"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("../foo"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str(":foo./"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str(":foo../"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/:foo\bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo/bar"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {

})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://example.com:8080/foo?bar#baz"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/foo?bar#baz"_kj, UrlPattern::CompileOptions {.baseUrl = "https://example.com:8080"_kj})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("http{s}?:"),.search = kj::str("?bar"),.hash = kj::str("#baz"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.search = kj::str("foo"),.baseUrl = kj::str("https://example.com/a/+/b"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hash = kj::str("foo"),.baseUrl = kj::str("https://example.com/?q=*&v=?&hmm={}&umm=()"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("#foo"_kj, UrlPattern::CompileOptions {.baseUrl = "https://example.com/?q=*&v=?&hmm={}&umm=()"_kj})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
  }
  KJ_CASE_ONEOF(err, kj::String) {
    KJ_FAIL_ASSERT("Failed to compile URLPattern", err);
  }
}
