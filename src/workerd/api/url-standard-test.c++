// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/jsg-test.h>
#include <kj/test.h>
#include "url-standard.h"
#include <workerd/jsg/setup.h>

namespace workerd::api::url {
namespace {

jsg::V8System v8System;
// We don't actually use V8 in this test, but we do use ICU, which needs to be initialized.
// Constructing a V8System will do that for us.

KJ_TEST("Minimal URL Parse") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse 2") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org/")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Username") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://abc@example.org/")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv("abc"));
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Username and Password") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://abc:xyz@example.org/")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv("abc"));
  KJ_ASSERT(record.password == jsg::usv("xyz"));
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Password, no Username") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://:xyz@example.org/")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv("xyz"));
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Port (non-default)") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org:123/")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  auto& port = KJ_ASSERT_NONNULL(record.port);
  KJ_ASSERT(port == 123);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Port (default)") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org:443/")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Port delimiter with no port)") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org:/")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - One path segment") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org/abc")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv("abc"));
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Leading single dot segment") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org/./abc")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv("abc"));
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Multiple single dot segment") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org/././././abc")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv("abc"));
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Leading double dot segment") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org/../abc")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv("abc"));
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Leading mixed dot segment") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org/../.././.././abc")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv("abc"));
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Three path segments") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org/a/b/c")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 3);
  KJ_ASSERT(path[0] == jsg::usv("a"));
  KJ_ASSERT(path[1] == jsg::usv("b"));
  KJ_ASSERT(path[2] == jsg::usv("c"));
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Three path segments with double dot") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org/a/b/../c")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 2);
  KJ_ASSERT(path[0] == jsg::usv("a"));
  KJ_ASSERT(path[1] == jsg::usv("c"));
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Three path segments with single dot") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org/a/b/./c")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 3);
  KJ_ASSERT(path[0] == jsg::usv("a"));
  KJ_ASSERT(path[1] == jsg::usv("b"));
  KJ_ASSERT(path[2] == jsg::usv("c"));
  KJ_ASSERT(record.query == nullptr);
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Query present but empty") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org?")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  auto& query = KJ_ASSERT_NONNULL(record.query);
  KJ_ASSERT(query == jsg::usv());
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Query minimal") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org?123")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  auto& query = KJ_ASSERT_NONNULL(record.query);
  KJ_ASSERT(query == jsg::usv("123"));
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Query minimal after missing port") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org:?123")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  auto& query = KJ_ASSERT_NONNULL(record.query);
  KJ_ASSERT(query == jsg::usv("123"));
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Query minimal after missing port and empty path") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org:/?123")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  auto& query = KJ_ASSERT_NONNULL(record.query);
  KJ_ASSERT(query == jsg::usv("123"));
  KJ_ASSERT(record.fragment == nullptr);
}

KJ_TEST("Minimal URL Parse - Fragment present but empty") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org#")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  KJ_ASSERT(record.query == nullptr);
  auto& fragment = KJ_ASSERT_NONNULL(record.fragment);
  KJ_ASSERT(fragment == jsg::usv());
}

KJ_TEST("Minimal URL Parse - Fragment minimal") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org#123")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  KJ_ASSERT(record.query == nullptr);
  auto& fragment = KJ_ASSERT_NONNULL(record.fragment);
  KJ_ASSERT(fragment == jsg::usv("123"));
}

KJ_TEST("Minimal URL Parse - Fragment minimal") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org?#123")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  auto& query = KJ_ASSERT_NONNULL(record.query);
  KJ_ASSERT(query == jsg::usv());
  auto& fragment = KJ_ASSERT_NONNULL(record.fragment);
  KJ_ASSERT(fragment == jsg::usv("123"));
}

KJ_TEST("Minimal URL Parse - Fragment minimal") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://example.org?abc#123")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv());
  KJ_ASSERT(record.password == jsg::usv());
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.port == nullptr);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 1);
  KJ_ASSERT(path[0] == jsg::usv());
  auto& query = KJ_ASSERT_NONNULL(record.query);
  KJ_ASSERT(query == jsg::usv("abc"));
  auto& fragment = KJ_ASSERT_NONNULL(record.fragment);
  KJ_ASSERT(fragment == jsg::usv("123"));
}

KJ_TEST("Minimal URL Parse - All together") {
  auto record =
      KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://abc:xyz@example.org:123/a/b/c?abc#123")));

  KJ_ASSERT(record.scheme == jsg::usv("https"));
  KJ_ASSERT(record.username == jsg::usv("abc"));
  KJ_ASSERT(record.password == jsg::usv("xyz"));
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  auto& port = KJ_ASSERT_NONNULL(record.port);
  KJ_ASSERT(port == 123);
  auto& path = KJ_ASSERT_NONNULL(record.path.tryGet<kj::Array<jsg::UsvString>>());
  KJ_ASSERT(path.size() == 3);
  KJ_ASSERT(path[0] == jsg::usv("a"));
  KJ_ASSERT(path[1] == jsg::usv("b"));
  KJ_ASSERT(path[2] == jsg::usv("c"));
  auto& query = KJ_ASSERT_NONNULL(record.query);
  KJ_ASSERT(query == jsg::usv("abc"));
  auto& fragment = KJ_ASSERT_NONNULL(record.fragment);
  KJ_ASSERT(fragment == jsg::usv("123"));
}

KJ_TEST("Minimal URL Parse - Not special (data URL)") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("data:something")));

  KJ_ASSERT(record.scheme == jsg::usv("data"));
  KJ_ASSERT(record.path.is<jsg::UsvString>());
  auto& path = record.path.get<jsg::UsvString>();
  KJ_ASSERT(path == jsg::usv("something"));
  KJ_ASSERT(!record.special);
}

KJ_TEST("Special scheme URLS") {
  jsg::UsvString tests[] = {
    jsg::usv("http://example.org"),
    jsg::usv("https://example.org"),
    jsg::usv("ftp://example.org"),
    jsg::usv("ws://example.org"),
    jsg::usv("wss://example.org"),
    jsg::usv("file:///example"),
  };

  for (auto n = 0; n < kj::size(tests); n++) {
    auto record = KJ_ASSERT_NONNULL(URL::parse(tests[n]));
    KJ_ASSERT(record.special);
  }
}

KJ_TEST("Trim leading and trailing control/space") {
  jsg::UsvStringBuilder builder;
  builder.add(' ', 0x0, 0x1, ' ');
  builder.addAll("http://example.org");
  builder.add(' ', 0x2, 0x3, ' ');

  auto record = KJ_ASSERT_NONNULL(URL::parse(builder.finish()));
  KJ_ASSERT(record.scheme == jsg::usv("http"));
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("example.org"));
  KJ_ASSERT(record.getPathname() == jsg::usv("/"));
}

KJ_TEST("Percent encoding in username/password") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://%66oo:%66oo@example.com/")));
  KJ_ASSERT(record.username == jsg::usv("%66oo"));
  KJ_ASSERT(record.password == jsg::usv("%66oo"));
}

KJ_TEST("Percent encoding in hostname") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://%66oo")));
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("foo"));
}

KJ_TEST("Percent encoding in hostname") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://%66oo")));
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("foo"));
}

KJ_TEST("Percent encoding in pathname") {
  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://example.org/%2e/%31%32%ZZ")));
    auto path = record.getPathname();
    // The %2e is properly detected as a single dot segment.
    // The invalid percent encoded %ZZ is ignored.
    KJ_ASSERT(path == jsg::usv("/%31%32%ZZ"));
  }
  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://example.org/%2e/%31%32%ZZ/%2E")));
    auto path = record.getPathname();
    KJ_ASSERT(path == jsg::usv("/%31%32%ZZ/"));
  }
}

KJ_TEST("Percent encoding in query") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://example.org?/%2e/%31%32%ZZ")));
  auto& query = KJ_ASSERT_NONNULL(record.query);
  // The invalid percent encoded %ZZ is ignored.
  KJ_ASSERT(query == jsg::usv("/%2e/%31%32%ZZ"));
}

KJ_TEST("Percent encoding in fragment") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://example.org#/%2e/%31%32%ZZ")));
  auto& fragment = KJ_ASSERT_NONNULL(record.fragment);
  // The invalid percent encoded %ZZ is ignored.
  KJ_ASSERT(fragment == jsg::usv("/%2e/%31%32%ZZ"));
}

KJ_TEST("Percent encoding of non-ascii characters in path, query, fragment") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://example.org/café?café#café")));
  KJ_ASSERT(record.getPathname() == jsg::usv("/caf%C3%A9"));
  auto& query = KJ_ASSERT_NONNULL(record.query);
  auto& fragment = KJ_ASSERT_NONNULL(record.fragment);
  KJ_ASSERT(query == jsg::usv("caf%C3%A9"));
  KJ_ASSERT(fragment == jsg::usv("caf%C3%A9"));
}

KJ_TEST("IDNA-conversion non-ascii characters in hostname") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://café.com")));
  auto& host = KJ_ASSERT_NONNULL(record.host);
  KJ_ASSERT(host == jsg::usv("xn--caf-dma.com"));
}

KJ_TEST("IPv4 in hostname") {
  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://123.210.123.121")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("123.210.123.121"));
  }

  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://2077391737")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("123.210.123.121"));
  }

  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://1.1")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("1.0.0.1"));
  }

  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://0x1.0x1")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("1.0.0.1"));
  }

  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://01.0x1")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("1.0.0.1"));
  }

  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://0x1000001")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("1.0.0.1"));
  }

  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://0100000001")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("1.0.0.1"));
  }

  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://192.168.1")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("192.168.0.1"));
  }

  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://192.0xa80001")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("192.168.0.1"));
  }

  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://192.11010049")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("192.168.0.1"));
  }

  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://0300.11010049")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("192.168.0.1"));
  }

  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://0300.0xa80001")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("192.168.0.1"));
  }

  {
    // Yes, this is a valid IPv4 address also.
    // You might be asking yourself, why would anyone do this?
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://0xc0.11010049")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("192.168.0.1"));
  }

  {
    KJ_ASSERT(URL::parse(jsg::usv("https://999.999.999.999")) == nullptr);
    KJ_ASSERT(URL::parse(jsg::usv("https://123.999.999.999")) == nullptr);
    KJ_ASSERT(URL::parse(jsg::usv("https://123.123.999.999")) == nullptr);
    KJ_ASSERT(URL::parse(jsg::usv("https://123.123.123.999")) == nullptr);
    KJ_ASSERT(URL::parse(jsg::usv("https://123.123.65536")) == nullptr);
    KJ_ASSERT(URL::parse(jsg::usv("https://123.16777216")) == nullptr);
    KJ_ASSERT(URL::parse(jsg::usv("https://4294967296")) == nullptr);
  }
}

KJ_TEST("IPv6 in hostname") {
  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://[1:1:1:1:1:1:1:1]")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("[1:1:1:1:1:1:1:1]"));
  }

  {
    // Compressed segments work
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://[1::1]")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("[1::1]"));
  }

  {
    // Compressed segments work
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://[::]")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("[::]"));
  }

  {
    // Normalized form is shortest, lowercase serialization.
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://[11:AF:0:0:0::0001]")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("[11:af::1]"));
  }

  {
    // IPv4-in-IPv6 syntax is supported
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("https://[2001:db8:122:344::192.0.2.33]")));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("[2001:db8:122:344::c000:221]"));
  }

  KJ_ASSERT(URL::parse(jsg::usv("https://[zz::top]")) == nullptr);
}

KJ_TEST("javascript: URLS") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("jAvAsCrIpT: alert('boo'); ")));
  KJ_ASSERT(record.scheme == jsg::usv("javascript"));
  KJ_ASSERT(record.getPathname() == jsg::usv(" alert('boo');"));
}

KJ_TEST("data: URLS") {
  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("data:,Hello%2C%20World%21")));
    KJ_ASSERT(record.scheme == jsg::usv("data"));
    KJ_ASSERT(record.getPathname() == jsg::usv(",Hello%2C%20World%21"));
  }
  {
    auto record =
        KJ_ASSERT_NONNULL(URL::parse(jsg::usv("data:text/plain;base64,SGVsbG8sIFdvcmxkIQ==")));
    KJ_ASSERT(record.scheme == jsg::usv("data"));
    KJ_ASSERT(record.getPathname() == jsg::usv("text/plain;base64,SGVsbG8sIFdvcmxkIQ=="));
  }
  {
    auto record =
        KJ_ASSERT_NONNULL(URL::parse(
            jsg::usv("data:text/html,%3Ch1%3EHello%2C%20World%21%3C%2Fh1%3E")));
    KJ_ASSERT(record.scheme == jsg::usv("data"));
    KJ_ASSERT(record.getPathname() == jsg::usv("text/html,%3Ch1%3EHello%2C%20World%21%3C%2Fh1%3E"));
  }
  {
    auto record =
        KJ_ASSERT_NONNULL(URL::parse(jsg::usv("data:text/html,<script>alert('hi');</script>")));
    KJ_ASSERT(record.scheme == jsg::usv("data"));
    KJ_ASSERT(record.getPathname() == jsg::usv("text/html,<script>alert('hi');</script>"));
  }
}

KJ_TEST("blob: URLS") {
  auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("blob:https://example.org")));
  KJ_ASSERT(record.scheme == jsg::usv("blob"));
  KJ_ASSERT(record.getPathname() == jsg::usv("https://example.org"));
}

KJ_TEST("Relative URLs") {
  {
    auto base =
        KJ_ASSERT_NONNULL(URL::parse(
            jsg::usv("https://abc:def@example.org:81/a/b/c?query#fragment")));
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv(), base));
    KJ_ASSERT(record.scheme == jsg::usv("https"));
    KJ_ASSERT(record.username == jsg::usv("abc"));
    KJ_ASSERT(record.password == jsg::usv("def"));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("example.org"));
    auto port = KJ_ASSERT_NONNULL(record.port);
    KJ_ASSERT(port == 81);
    KJ_ASSERT(record.getPathname() == jsg::usv("/a/b/c"));
    auto& query = KJ_ASSERT_NONNULL(record.query);
    KJ_ASSERT(query == jsg::usv("query"));
    KJ_ASSERT(record.fragment == nullptr);
  }

  {
    auto base =
        KJ_ASSERT_NONNULL(URL::parse(
            jsg::usv("https://abc:def@example.org:81/a/b/c?query#fragment")));
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("/xyz"), base));
    KJ_ASSERT(record.scheme == jsg::usv("https"));
    KJ_ASSERT(record.username == jsg::usv("abc"));
    KJ_ASSERT(record.password == jsg::usv("def"));
    auto& host = KJ_ASSERT_NONNULL(record.host);
    KJ_ASSERT(host == jsg::usv("example.org"));
    auto port = KJ_ASSERT_NONNULL(record.port);
    KJ_ASSERT(port == 81);
    KJ_ASSERT(record.getPathname() == jsg::usv("/xyz"));
    KJ_ASSERT(record.query == nullptr);
    KJ_ASSERT(record.fragment == nullptr);
  }

  {
    auto base =
        KJ_ASSERT_NONNULL(URL::parse(
            jsg::usv("https://abc:def@example.org:81/a/b/c?query#fragment")));
    auto record =
        KJ_ASSERT_NONNULL(URL::parse(jsg::usv("../../../../../././../../././../.././abc"), base));
    KJ_ASSERT(record.getPathname() == jsg::usv("/abc"));
  }

  {
    auto base = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("data:cannot-be-base")));
    auto record = URL::parse(jsg::usv("/anything"), base);
    KJ_ASSERT(record == nullptr);
  }
}

KJ_TEST("Parse protocol with state override") {
  {
    auto record = KJ_ASSERT_NONNULL(URL::parse(jsg::usv("http://example.org")));
    record = KJ_ASSERT_NONNULL(URL::parse(
        jsg::usv("http:"), nullptr, record, URL::ParseState::SCHEME_START));
    KJ_ASSERT(record.scheme == jsg::usv("http"));
  }
}

}  // namespace
}  // namespace workerd::api::url

