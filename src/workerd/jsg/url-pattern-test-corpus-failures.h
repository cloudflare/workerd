
// Per the Web Platform Tests, all of these should fail to compile successfully

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.protocol = kj::str("(café)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.username = kj::str("(café)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.password = kj::str("(café)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("(café)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("(café)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.search = kj::str("(café)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hash = kj::str("(café)"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("example.com/foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("data:foobar"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://{sub.}?example{.com/}foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("{https://}example.com/foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("(https://)example.com/foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://{sub{.}}example.com/foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("(café)://foo"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("[\\:\\:xY\\::num]"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("*\\:1]"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://foo{{@}}example.com"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://foo{@example.com"_kj)) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/:id/:id"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.pathname = kj::str("/foo"), .baseUrl = kj::str(""),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/foo"_kj, UrlPattern::CompileOptions {
  .baseUrl = ""_kj
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad#hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad%hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad/hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad\\:hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad<hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad>hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad?hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad@hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad[hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad]hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad\\\\hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad^hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad|hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad\nhostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad\rhostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("bad	hostname"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("{[\\:\\:fé\\::num]}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}

KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
.hostname = kj::str("{[\\:\\::num\\:fé]}"),
})) {
  KJ_CASE_ONEOF(pattern, UrlPattern) {
    KJ_FAIL_ASSERT("Test case should have failed");
  }
  KJ_CASE_ONEOF(err, kj::String) {
    // ok!
  }
}
