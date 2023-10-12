// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"
#include "url.h"
#include <kj/table.h>
#include <regex>
#include <openssl/rand.h>

namespace workerd::jsg::test {
namespace {

KJ_TEST("Basics") {
  Url theUrl = nullptr;
  KJ_IF_SOME(url, Url::tryParse("http://example.org:81"_kj)) {
    KJ_ASSERT(url.getOrigin() == "http://example.org:81"_kj);
    KJ_ASSERT(url.getHref() == "http://example.org:81/"_kj);
    KJ_ASSERT(url.getProtocol() == "http:"_kj);
    KJ_ASSERT(url.getHostname() == "example.org"_kj);
    KJ_ASSERT(url.getHost() == "example.org:81"_kj);
    KJ_ASSERT(url.getPort() == "81"_kj);
    KJ_ASSERT(url.getPathname() == "/"_kj);
    KJ_ASSERT(url.getSchemeType() == Url::SchemeType::HTTP);
    KJ_ASSERT(url.getHostType() == Url::HostType::DEFAULT);
    KJ_ASSERT(url.getUsername() == ""_kj);
    KJ_ASSERT(url.getPassword() == ""_kj);
    KJ_ASSERT(url.getHash() == ""_kj);
    KJ_ASSERT(url.getSearch() == ""_kj);

    theUrl = url.clone();
    KJ_ASSERT(theUrl == url);
    theUrl = kj::mv(url);

    auto res = KJ_ASSERT_NONNULL(theUrl.resolve("abc"_kj));
    KJ_ASSERT(res.getHref() == "http://example.org:81/abc"_kj);

    // jsg::Urls support KJ_STRINGIFY
    KJ_ASSERT(kj::str(res) == "http://example.org:81/abc");

    // jsg::Urls are suitable to be used as keys in a hashset, hashmap
    kj::HashSet<Url> urls;
    urls.insert(res.clone());
    KJ_ASSERT(urls.contains(res));

    kj::HashMap<Url, int> urlmap;
    urlmap.insert(res.clone(), 1);
    KJ_ASSERT(KJ_ASSERT_NONNULL(urlmap.find(res)) == 1);
  } else {
    KJ_FAIL_ASSERT("url could not be parsed");
  }

  KJ_ASSERT(Url::idnToAscii("täst.de"_kj) == "xn--tst-qla.de"_kj);
  KJ_ASSERT(Url::idnToUnicode("xn--tst-qla.de"_kj) == "täst.de"_kj);
}

KJ_TEST("Non-special URL") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("abc://123"_kj));
  KJ_ASSERT(url.getOrigin() == "null"_kj);
  KJ_ASSERT(url.getProtocol() == "abc:"_kj);
}

KJ_TEST("Invalid Urls") {
#include "url-test-corpus-failures.h"
}

void test(kj::StringPtr input,
          kj::Maybe<kj::StringPtr> base,
          kj::StringPtr href) {
  KJ_ASSERT(Url::canParse(input, base));
  auto url = KJ_ASSERT_NONNULL(Url::tryParse(input, base));
  KJ_ASSERT(url.getHref() == href);
}

KJ_TEST("Valid Urls") {
  KJ_ASSERT_NONNULL(Url::tryParse(""_kj, "http://example.org"_kj));
#include "url-test-corpus-success.h"
}

KJ_TEST("Search params (1)") {
  UrlSearchParams params;
  params.append("foo"_kj, "bar"_kj);
  KJ_ASSERT(params.toStr() == "foo=bar"_kj);
}

KJ_TEST("Search params (2)") {
  auto params = KJ_ASSERT_NONNULL(UrlSearchParams::tryParse("foo=bar&a=b&a=c"_kj));
  KJ_ASSERT(params.has("a"_kj));
  KJ_ASSERT(params.has("foo"_kj, kj::Maybe("bar"_kj)));
  KJ_ASSERT(!params.has("foo"_kj, kj::Maybe("baz"_kj)));
  KJ_ASSERT(KJ_ASSERT_NONNULL(params.get("a"_kj)) == "b"_kj);

  auto all = params.getAll("a"_kj);
  KJ_ASSERT(all.size() == 2);
  KJ_ASSERT(all[0] == "b"_kj);
  KJ_ASSERT(all[1] == "c"_kj);

  params.delete_("foo"_kj);
  params.delete_("a"_kj, kj::Maybe("c"_kj));

  params.set("a"_kj, "z"_kj);
  KJ_ASSERT(kj::str(params) == "a=z");
}

// ======================================================================================

KJ_TEST("URLPattern - processInit Default") {
  KJ_SWITCH_ONEOF(UrlPattern::processInit(UrlPattern::Init {})) {
    KJ_CASE_ONEOF(result, UrlPattern::Init) {
      KJ_ASSERT(result.baseUrl == kj::none);
      KJ_ASSERT(result.protocol == kj::none);
      KJ_ASSERT(result.username == kj::none);
      KJ_ASSERT(result.password == kj::none);
      KJ_ASSERT(result.hostname == kj::none);
      KJ_ASSERT(result.port == kj::none);
      KJ_ASSERT(result.pathname == kj::none);
      KJ_ASSERT(result.search == kj::none);
      KJ_ASSERT(result.hash == kj::none);
    }
    KJ_CASE_ONEOF(msg, kj::String) {
      KJ_FAIL_ASSERT("Default processInit failed", msg);
    }
  }
}

KJ_TEST("URLPattern - processInit PATTERN mode") {
  KJ_SWITCH_ONEOF(UrlPattern::processInit(UrlPattern::Init {
    // Since we're using PATTERN mode here (the default), the values
    // for each field will not be canonicalized.
    .protocol = kj::str("something"),
    .username = kj::str("something"),
    .password = kj::str("something"),
    .hostname = kj::str("something"),
    .port = kj::str("something"),
    .pathname = kj::str("something"),
    .search = kj::str("something"),
    .hash = kj::str("something"),
  })) {
    KJ_CASE_ONEOF(result, UrlPattern::Init) {
      KJ_ASSERT(result.baseUrl == kj::none);
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.protocol) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.hostname) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.pathname) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.username) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.password) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.port) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.search) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.hash) == "something");
    }
    KJ_CASE_ONEOF(msg, kj::String) {
      KJ_FAIL_ASSERT("processInit failed", msg);
    }
  }
}

KJ_TEST("URLPattern - processInit PATTERN mode") {
  KJ_SWITCH_ONEOF(UrlPattern::processInit(UrlPattern::Init {
    // Since we're using PATTERN mode here (the default), the values
    // for each field will not be canonicalized.
    .protocol = kj::str("something"),
    .username = kj::str("something"),
    .password = kj::str("something"),
    .hostname = kj::str("something"),
    .pathname = kj::str("something"),
    .hash = kj::str("something"),
  }, UrlPattern::ProcessInitOptions {
    .port = "something"_kj,
    .search = "something"_kj,
  })) {
    KJ_CASE_ONEOF(result, UrlPattern::Init) {
      KJ_ASSERT(result.baseUrl == kj::none);
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.protocol) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.hostname) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.pathname) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.username) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.password) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.port) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.search) == "something");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.hash) == "something");
    }
    KJ_CASE_ONEOF(msg, kj::String) {
      KJ_FAIL_ASSERT("processInit failed", msg);
    }
  }
}

KJ_TEST("URLPattern - processInit base") {
  KJ_SWITCH_ONEOF(UrlPattern::processInit(UrlPattern::Init {
    .baseUrl = kj::str("https://example.org")
  })) {
    KJ_CASE_ONEOF(result, UrlPattern::Init) {
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.baseUrl) == "https://example.org");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.protocol) == "https");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.hostname) == "example.org");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.pathname) == "/");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.username) == "");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.password) == "");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.port) == "");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.search) == "");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.hash) == "");
    }
    KJ_CASE_ONEOF(msg, kj::String) {
      KJ_FAIL_ASSERT("Default processInit failed", msg);
    }
  }
}

KJ_TEST("URLPattern - processInit base relative path") {
  KJ_SWITCH_ONEOF(UrlPattern::processInit(UrlPattern::Init {
    .pathname = kj::str("d"),
    .baseUrl = kj::str("https://example.org/a/b/c"),
  }, UrlPattern::ProcessInitOptions {
    .port = "1234"_kj
  })) {
    KJ_CASE_ONEOF(result, UrlPattern::Init) {
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.baseUrl) == "https://example.org/a/b/c");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.protocol) == "https");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.hostname) == "example.org");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.pathname) == "/a/b/d");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.username) == "");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.password) == "");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.port) == "1234");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.search) == "");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.hash) == "");
    }
    KJ_CASE_ONEOF(msg, kj::String) {
      KJ_FAIL_ASSERT("Default processInit failed", msg);
    }
  }
}

KJ_TEST("URLPattern - processInit invalid base") {
  KJ_SWITCH_ONEOF(UrlPattern::processInit(UrlPattern::Init {
    .baseUrl = kj::str("not a valid url")
  })) {
    KJ_CASE_ONEOF(result, UrlPattern::Init) {
      KJ_FAIL_ASSERT("processInit should have failed");
    }
    KJ_CASE_ONEOF(msg, kj::String) {
      KJ_ASSERT(msg == "Invalid base URL.");
    }
  }
}

KJ_TEST("URLPattern - processInit URL mode (default)") {
  KJ_SWITCH_ONEOF(UrlPattern::processInit(UrlPattern::Init {
  }, UrlPattern::ProcessInitOptions {
    .mode = UrlPattern::ProcessInitOptions::Mode::URL
  })) {
    KJ_CASE_ONEOF(result, UrlPattern::Init) {
      KJ_ASSERT(result.protocol == kj::none);
    }
    KJ_CASE_ONEOF(str, kj::String) {
      KJ_FAIL_ASSERT("processInit URL mode failed", str);
    }
  }
}

KJ_TEST("URLPattern - processInit URL mode (protocol, fake)") {
  KJ_SWITCH_ONEOF(UrlPattern::processInit(UrlPattern::Init {
    // The value will be canonicalized
    .protocol = kj::str(" FaKe"),
    .username = kj::str("  mE!:  "),
    .password = kj::str(" @@@:@@@"),
    .hostname = kj::str("FOO.bar."),
    .port = kj::str("123"),
    .pathname = kj::str("d"),
    .search = kj::str("?yabba dabba doo"),
    .hash = kj::str("# "),
    .baseUrl = kj::str("http://ignored/a/b/c"),
  }, UrlPattern::ProcessInitOptions {
    .mode = UrlPattern::ProcessInitOptions::Mode::URL
  })) {
    KJ_CASE_ONEOF(result, UrlPattern::Init) {
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.protocol) == "fake");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.username) == "%20%20mE!%3A%20%20");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.password) == "%20%40%40%40%3A%40%40%40");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.hostname) == "FOO.bar.");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.port) == "123");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.pathname) == "/d");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.search) == "yabba%20dabba%20doo");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.hash) == "%20");
    }
    KJ_CASE_ONEOF(str, kj::String) {
      KJ_FAIL_ASSERT("processInit URL mode failed", str);
    }
  }
}

KJ_TEST("URLPattern - processInit URL mode (protocol, http)") {
  KJ_SWITCH_ONEOF(UrlPattern::processInit(UrlPattern::Init {
    .username = kj::str("  mE!:  "),
    .password = kj::str(" @@@:@@@"),
    .hostname = kj::str("123"),
    .port = kj::str("80"),
    .pathname = kj::str("d"),
    .search = kj::str("?yabba dabba doo"),
    .hash = kj::str("# "),
    .baseUrl = kj::str("http://something/a/b/c"),
  }, UrlPattern::ProcessInitOptions {
    .mode = UrlPattern::ProcessInitOptions::Mode::URL
  })) {
    KJ_CASE_ONEOF(result, UrlPattern::Init) {
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.protocol) == "http");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.username) == "%20%20mE!%3A%20%20");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.password) == "%20%40%40%40%3A%40%40%40");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.hostname) == "0.0.0.123");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.port) == "");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.pathname) == "/a/b/d");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.search) == "yabba%20dabba%20doo");
      KJ_ASSERT(KJ_ASSERT_NONNULL(result.hash) == "%20");
    }
    KJ_CASE_ONEOF(str, kj::String) {
      KJ_FAIL_ASSERT("processInit URL mode failed", str);
    }
  }
}

KJ_TEST("URLPattern - compile with empty init") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {})) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      // In this case, with an empty Init, all of the components should be
      // interpreted as wildcards capable of matching any input.
#define CHECK(Name)                                      \
  KJ_ASSERT(pattern.get##Name().getPattern() == "*");    \
  KJ_ASSERT(pattern.get##Name().getRegex() == "^(.*)$"); \
  KJ_ASSERT(pattern.get##Name().getNames().size() == 1); \
  KJ_ASSERT(pattern.get##Name().getNames()[0] == "0");
      CHECK(Protocol);
      CHECK(Username);
      CHECK(Password);
      CHECK(Hostname);
      CHECK(Port);
      CHECK(Pathname);
      CHECK(Search);
      CHECK(Hash);
#undef CHECK
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("Compiling the empty pattern failed", err);
    }
  }
}

KJ_TEST("URLPattern - compile with all wildcard init") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
    .protocol = kj::str("*"),
    .username = kj::str("*"),
    .password = kj::str("*"),
    .hostname = kj::str("*"),
    .port = kj::str("*"),
    .pathname = kj::str("*"),
    .search = kj::str("*"),
    .hash = kj::str("*"),
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      // In this case, with an empty Init, all of the components should be
      // interpreted as wildcards capable of matching any input.
#define CHECK(Name)                                      \
  KJ_ASSERT(pattern.get##Name().getPattern() == "*");    \
  KJ_ASSERT(pattern.get##Name().getRegex() == "^(.*)$"); \
  KJ_ASSERT(pattern.get##Name().getNames().size() == 1); \
  KJ_ASSERT(pattern.get##Name().getNames()[0] == "0");
      CHECK(Protocol);
      CHECK(Username);
      CHECK(Password);
      CHECK(Hostname);
      CHECK(Port);
      CHECK(Pathname);
      CHECK(Search);
      CHECK(Hash);
#undef CHECK
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("Compiling the wildcard pattern failed", err);
    }
  }
}

KJ_TEST("URLPattern - compile with http protocol only") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
    .protocol = kj::str("http"),
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      auto& protocol = pattern.getProtocol();
      KJ_ASSERT(protocol.getPattern() == "http");
      KJ_ASSERT(protocol.getRegex() == "^http$");
      KJ_ASSERT(protocol.getNames().size() == 0);
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("Compiling the empty pattern failed", err);
    }
  }
}

KJ_TEST("URLPattern - compile with http{s}? protocol") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
    .protocol = kj::str("http{s}?"),
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      auto& protocol = pattern.getProtocol();
      KJ_ASSERT(protocol.getPattern() == "http{s}?");
      KJ_ASSERT(protocol.getRegex() == "^http(?:s)?$");
      KJ_ASSERT(protocol.getNames().size() == 0);
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("Compiling the empty pattern failed", err);
    }
  }
}

KJ_TEST("URLPattern - compile with http{s}+ protocol") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
    .protocol = kj::str("http{s}+"),
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      auto& protocol = pattern.getProtocol();
      KJ_ASSERT(protocol.getPattern() == "http{s}+");
      KJ_ASSERT(protocol.getRegex() == "^http(?:s)+$");
      KJ_ASSERT(protocol.getNames().size() == 0);
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("Compiling the empty pattern failed", err);
    }
  }
}

KJ_TEST("URLPattern - compile with http{s}* protocol") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
    .protocol = kj::str("http{s}*"),
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      auto& protocol = pattern.getProtocol();
      KJ_ASSERT(protocol.getPattern() == "http{s}*");
      KJ_ASSERT(protocol.getRegex() == "^http(?:s)*$");
      KJ_ASSERT(protocol.getNames().size() == 0);
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("Compiling the empty pattern failed", err);
    }
  }
}

KJ_TEST("URLPattern - compile with http(s)? protocol") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
    .protocol = kj::str("http(.)?"),
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      auto& protocol = pattern.getProtocol();
      KJ_ASSERT(protocol.getPattern() == "http(.)?");
      KJ_ASSERT(protocol.getRegex() == "^http(.)?$");
      KJ_ASSERT(protocol.getNames().size() == 1);
      KJ_ASSERT(protocol.getNames()[0] == "0");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("Compiling the empty pattern failed", err);
    }
  }
}

KJ_TEST("URLPattern - compile with :foo:bar protocol") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
    .protocol = kj::str(":foo:bar"),
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      auto& protocol = pattern.getProtocol();
      KJ_ASSERT(protocol.getPattern() == ":foo:bar");
      KJ_ASSERT(protocol.getRegex() == "^([^]+)([^]+)$");
      KJ_ASSERT(protocol.getNames().size() == 2);
      KJ_ASSERT(protocol.getNames()[0] == "foo");
      KJ_ASSERT(protocol.getNames()[1] == "bar");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("Compiling the empty pattern failed", err);
    }
  }
}

KJ_TEST("URLPattern - compile with :foo(http) protocol") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
    .protocol = kj::str(":foo(http)"),
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      auto& protocol = pattern.getProtocol();
      KJ_ASSERT(protocol.getPattern() == ":foo(http)");
      KJ_ASSERT(protocol.getRegex() == "^(http)$");
      KJ_ASSERT(protocol.getNames().size() == 1);
      KJ_ASSERT(protocol.getNames()[0] == "foo");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("Compiling the empty pattern failed", err);
    }
  }
}

KJ_TEST("URLPattern - compile with :foo(http{s}?) protocol") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile(UrlPattern::Init {
    .protocol = kj::str(":foo(http[s]?)"),
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      auto& protocol = pattern.getProtocol();
      KJ_ASSERT(protocol.getPattern() == ":foo(http[s]?)");
      KJ_ASSERT(protocol.getRegex() == "^(http[s]?)$");
      KJ_ASSERT(protocol.getNames().size() == 1);
      KJ_ASSERT(protocol.getNames()[0] == "foo");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("Compiling the empty pattern failed", err);
    }
  }
}

KJ_TEST("URLPattern - compile from empty string") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("")) {
    KJ_CASE_ONEOF(init, UrlPattern) {
      KJ_FAIL_ASSERT("URL pattern compile should have failed");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_ASSERT(err == "Syntax error in URL Pattern: a relative pattern must have a base URL.");
    }
  }
}

KJ_TEST("URLPattern - compile from empty string with base") {
  static constexpr auto BASEURL = "http://example.com"_kjc;
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("", UrlPattern::CompileOptions {
    .baseUrl = BASEURL,
  })) {
    KJ_CASE_ONEOF(init, UrlPattern) {
      // ok!
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("Parsing from empty string with a base URL failed", err);
    }
  }
}

KJ_TEST("URLPattern - compile from http{s}?: string") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("http{s}?:")) {
    KJ_CASE_ONEOF(init, UrlPattern) {
      KJ_ASSERT(init.getProtocol().getPattern() == "http{s}?");
      KJ_ASSERT(init.getProtocol().getRegex() == "^http(?:s)?$");
      KJ_ASSERT(init.getProtocol().getNames().size() == 0);
      KJ_ASSERT(init.getUsername().getPattern() == "");
      KJ_ASSERT(init.getUsername().getRegex() == "^$");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("Failed to parse empty string", err);
    }
  }
}

bool testPattern(kj::StringPtr regex,
                 kj::ArrayPtr<const char> input,
                 kj::ArrayPtr<kj::String> groups = nullptr) {
  std::regex rx(regex.begin(), regex.size());
  std::cmatch match;
  bool result = std::regex_match(input.begin(), input.end(), match, rx);
  if (result && groups != nullptr) {
    KJ_ASSERT(match.size() == (groups.size() + 1));
    for (int n = 1; n < match.size(); n++) {
      KJ_ASSERT(groups[n-1] == match[n].str().c_str());
    }
  }
  return result;
}

KJ_TEST("URLPattern - MDN example 1 - pathname: '/books'") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/books")
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.com/books"_kj));
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), url.getProtocol()));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), url.getHostname()));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), url.getPathname()));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 2 - pathname: '/books/:id'") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/books/:id")
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.com/books/123"_kj));
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), url.getProtocol()));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), url.getHostname()));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), url.getPathname(),
                            kj::arr(kj::str("123"))));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 3 - pathname: '/books/:id(\\d+)' with base") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/books/:id(\\d+)"_kj, UrlPattern::CompileOptions {
    .baseUrl = "https://example.com"_kj
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.com/books/123"_kj));
      auto protocol = url.getProtocol();
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(),
                            protocol.slice(0, protocol.size() - 1)));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), url.getHostname()));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), url.getPathname(),
                            kj::arr(kj::str("123"))));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books/abc"_kj));
      KJ_ASSERT(pattern.getPathname().getNames().size() == 1);
      KJ_ASSERT(pattern.getPathname().getNames()[0] == "id");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 4 - pathame: '/:type(foo|bar)'") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/:type(foo|bar)")
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/foo"_kj, kj::arr(kj::str("foo"))));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/bar"_kj, kj::arr(kj::str("bar"))));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/baz"_kj));
      KJ_ASSERT(pattern.getPathname().getNames().size() == 1);
      KJ_ASSERT(pattern.getPathname().getNames()[0] == "type");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 5 - '/books/:id(\\d+) with base") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/books/:id(\\d+)"_kj, UrlPattern::CompileOptions {
    .baseUrl = "https://example.com"_kj
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123"_kj,
                            kj::arr(kj::str("123"))));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books/abc"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 6 - '/books/(\\d+)' with base") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/books/(\\d+)"_kj, UrlPattern::CompileOptions {
    .baseUrl = "https://example.com"_kj
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123"_kj,
                            kj::arr(kj::str("123"))));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books/abc"_kj));
      KJ_ASSERT(pattern.getPathname().getNames()[0] == "0");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 7 - '/books/:id?' with base") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/books/:id?", UrlPattern::CompileOptions {
    .baseUrl = "https://example.com"_kj
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "https"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books/"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books/123/456"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books/123/456/789"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 8 - '/books/:id+' with base") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/books/:id+", UrlPattern::CompileOptions {
    .baseUrl = "https://example.com"_kj
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "https"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books/"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123/456"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123/456/789"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 9 - '/books/:id*' with base") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/books/:id*", UrlPattern::CompileOptions {
    .baseUrl = "https://example.com"_kj
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "https"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books/"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123/456"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123/456/789"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 10 - '/book{s}?' with base") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/book{s}?", UrlPattern::CompileOptions {
    .baseUrl = "https://example.com"_kj
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "https"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/book"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books/"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 11 - '/book{s}' with base") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/book{s}", UrlPattern::CompileOptions {
    .baseUrl = "https://example.com"_kj
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "https"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/book"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books/"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 12 - '/blog/:id(\\d+){-:title}?'") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/blog/:id(\\d+){-:title}?")
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "https"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/blog/123-my-blog"_kj),
                kj::arr(kj::str("123"), kj::str("my-blog")));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/blog/123"_kj),
                kj::arr(kj::str("123"), kj::str()));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/blog/my-blog"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 13 - '/books/:id?' with base") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/books/:id?"_kj, UrlPattern::CompileOptions {
    .baseUrl = "https://example.com"_kj
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "https"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123"_kj),
                kj::arr(kj::str("123")));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books"_kj), kj::arr(kj::str()));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books/"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 14 - '/books/:id+' with base") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/books/:id+"_kj, UrlPattern::CompileOptions {
    .baseUrl = "https://example.com/abc"_kj
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "https"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123"_kj,
                kj::arr(kj::str("123"))));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123/456"_kj,
                kj::arr(kj::str("123/456"))));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 15 - { hash: '/books/:id?' }") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({ .hash = kj::str("/books/:id?") })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "https"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/"_kj));
      KJ_ASSERT(testPattern(pattern.getHash().getRegex(), "/books/123"_kj));
      KJ_ASSERT(testPattern(pattern.getHash().getRegex(), "/books/"_kj));
      KJ_ASSERT(!testPattern(pattern.getHash().getRegex(), "/books"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 16 - { pathname: '/books/{:id}?' }") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({ .pathname = kj::str("/books/{:id}?") })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 17 - '/books/*' with base") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/books/*"_kj, UrlPattern::CompileOptions {
    .baseUrl = "https://example.com"_kj
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/books"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/books/123/456"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 18 - '/books/*' with base") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("/*.png"_kj, UrlPattern::CompileOptions {
    .baseUrl = "https://example.com"_kj
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/image.png"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/folder/image.png"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/.png"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/image.png/123"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 19 - hostname '{*.}?example.com'") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .hostname = kj::str("{*.}?example.com")
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj,
                            kj::arr(kj::str(""))));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "www.example.com"_kj,
                            kj::arr(kj::str("www"))));
      KJ_ASSERT(!testPattern(pattern.getHostname().getRegex(), "example.org"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 20") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("https://cdn-*.example.com/*.jpg"_kj)) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "https"_kj));
      KJ_ASSERT(!testPattern(pattern.getProtocol().getRegex(), "http"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "cdn-1234.example.com"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "cdn-foo.bar.example.com"_kj));
      KJ_ASSERT(!testPattern(pattern.getHostname().getRegex(), "cdn.example.com"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/image.jpg"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/stuff/image.jpg"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/stuff/image.gif"_kj));
      KJ_ASSERT(pattern.getProtocol().getPattern() == "https");
      KJ_ASSERT(pattern.getHostname().getPattern() == "cdn-*.example.com");
      KJ_ASSERT(pattern.getPathname().getPattern() == "/*.jpg");
      KJ_ASSERT(pattern.getSearch().getPattern() == "");
      KJ_ASSERT(pattern.getHash().getPattern() == "");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 21") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("data:foo*"_kj)) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_FAIL_ASSERT("URL Pattern compile should have failed");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_ASSERT(err == "Syntax error in URL Pattern: a relative pattern must have a base URL.");
    }
  }
}

KJ_TEST("URLPattern - MDN example 22") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .hostname = kj::str("example.com"),
    .pathname = kj::str("/foo/*"),
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/foo"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/foo/"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/foo/bar"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 23") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/foo/*"),
    .baseUrl = kj::str("https://example.com")
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "https"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/foo"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/foo/"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/foo/bar"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 24") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .hostname = kj::str("*.example.com")
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(!testPattern(pattern.getHostname().getRegex(), "example.com"_kj));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "www.example.com"_kj),
                kj::arr(kj::str("www")));
      KJ_ASSERT(pattern.getHostname().getNames()[0] == "0");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 25") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/:product/:user/:action")
 })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/store/wanderview/view"_kj),
                kj::arr(kj::str("store"), kj::str("wanderview"), kj::str("view")));
      KJ_ASSERT(pattern.getPathname().getNames().size() == 3);
      KJ_ASSERT(pattern.getPathname().getNames()[0] == "product");
      KJ_ASSERT(pattern.getPathname().getNames()[1] == "user");
      KJ_ASSERT(pattern.getPathname().getNames()[2] == "action");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 26") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/:product/:action+")
 })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product/do/something/cool"_kj),
                kj::arr(kj::str("product"), kj::str("do/something/cool")));
      KJ_ASSERT(pattern.getPathname().getNames().size() == 2);
      KJ_ASSERT(pattern.getPathname().getNames()[0] == "product");
      KJ_ASSERT(pattern.getPathname().getNames()[1] == "action");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 27") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/:product/:action*")
 })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product/do/something/cool"_kj),
                kj::arr(kj::str("product"), kj::str("do/something/cool")));
      KJ_ASSERT(pattern.getPathname().getNames().size() == 2);
      KJ_ASSERT(pattern.getPathname().getNames()[0] == "product");
      KJ_ASSERT(pattern.getPathname().getNames()[1] == "action");
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product"_kj),
                kj::arr(kj::str("product"), kj::str("")));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/product/"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 28") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .hostname = kj::str("{:subdomain.}*example.com")
 })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(pattern.getHostname().getNames().size() == 1);
      KJ_ASSERT(pattern.getHostname().getNames()[0] == "subdomain");
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj,
                kj::arr(kj::str(""))));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "foo.bar.example.com"_kj,
                kj::arr(kj::str("foo.bar"))));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 29") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/product{/}?")
 })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product/"_kj));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/product/abc"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 32") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
      .protocol = kj::str("http{s}?"),
      .username = kj::str(":user?"),
      .password = kj::str(":pass?"),
      .hostname = kj::str("{:subdomain.}*example.com"),
      .pathname = kj::str("/product/:action*"),
 })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(!testPattern(pattern.getProtocol().getRegex(), "ftp"_kj));
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "http"_kj));
      KJ_ASSERT(testPattern(pattern.getProtocol().getRegex(), "https"_kj));

      KJ_ASSERT(testPattern(pattern.getUsername().getRegex(), "foo"_kj, kj::arr(kj::str("foo"))));
      KJ_ASSERT(testPattern(pattern.getPassword().getRegex(), "bar"_kj, kj::arr(kj::str("bar"))));

      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "example.com"_kj,
                            kj::arr(kj::str(""))));
      KJ_ASSERT(testPattern(pattern.getHostname().getRegex(), "www.example.com"_kj,
                            kj::arr(kj::str("www"))));

      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product/bar"_kj,
                            kj::arr(kj::str("bar"))));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product"_kj,
                            kj::arr(kj::str())));

    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 33") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile("data\\:foo*"_kj)) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(pattern.getProtocol().getPattern() == "data");
      KJ_ASSERT(pattern.getProtocol().getRegex() == "^data$");
      KJ_ASSERT(pattern.getPathname().getPattern() == "foo*");
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "foobar"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 34") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/(foo|bar)")
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/foo"_kj, kj::arr(kj::str("foo"))));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/bar"_kj, kj::arr(kj::str("bar"))));
      KJ_ASSERT(!testPattern(pattern.getPathname().getRegex(), "/baz"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 35a") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/product/(index.html)?")
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product/index.html"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 35b") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/product/:action?")
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product/view"_kj,
                            kj::arr(kj::str("view"))));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product"_kj,
                            kj::arr(kj::str())));
      KJ_ASSERT(pattern.getPathname().getNames()[0] == "action");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - MDN example 35c") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
    .pathname = kj::str("/product/*?")
  })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product/wanderview/view"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product/"_kj));
      KJ_ASSERT(testPattern(pattern.getPathname().getRegex(), "/product"_kj));
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }
}

KJ_TEST("URLPattern - fun") {
  KJ_SWITCH_ONEOF(UrlPattern::tryCompile(":café://:ሴ/:_✔️"_kj)) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_ASSERT(pattern.getProtocol().getPattern() == ":café"_kj);
      KJ_ASSERT(pattern.getHostname().getPattern() == ":ሴ"_kj);
      KJ_ASSERT(pattern.getPathname().getPattern() == "/:_%E2%9C%94%EF%B8%8F");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      KJ_FAIL_ASSERT("URL Pattern compile failed", err);
    }
  }

  KJ_SWITCH_ONEOF(UrlPattern::tryCompile({ .hash = kj::str("=((") })) {
    KJ_CASE_ONEOF(pattern, UrlPattern) {
      KJ_FAIL_ASSERT("Parsing should have failed");
    }
    KJ_CASE_ONEOF(err, kj::String) {
      // ok! This is what we would expect. The exact error is not important here,
      // we just want to make sure this does not crash.
    }
  }
}

KJ_TEST("URLPattern - simple fuzzing") {
  for (int n = 1; n < 100; n++) {
    kj::Array<kj::byte> bufs = kj::heapArray<kj::byte>(9 * n);
    RAND_bytes(bufs.begin(), 9 * n);
    // We don't care if the compiling passes or fails, we just don't want crashes.
    KJ_SWITCH_ONEOF(UrlPattern::tryCompile({
      .protocol = kj::str(bufs.slice(0, n )),
      .username = kj::str(bufs.slice(n, n * 2)),
      .password = kj::str(bufs.slice(n * 2, n * 3)),
      .hostname = kj::str(bufs.slice(n * 3, n * 4)),
      .port = kj::str(bufs.slice(n * 4, n * 5)),
      .pathname = kj::str(bufs.slice(n * 5, n * 6)),
      .search = kj::str(bufs.slice(n * 6, n * 7)),
      .hash = kj::str(bufs.slice(n * 7, n * 8)),
      .baseUrl = kj::str(bufs.slice(n * 8, n * 9))
    })) {
      KJ_CASE_ONEOF(str, kj::String) {}
      KJ_CASE_ONEOF(pattern, UrlPattern) {}
    }

    auto input = kj::str(bufs);
    KJ_SWITCH_ONEOF(UrlPattern::tryCompile(input.asPtr())) {
      KJ_CASE_ONEOF(str, kj::String) {}
      KJ_CASE_ONEOF(pattern, UrlPattern) {}
    }

    KJ_SWITCH_ONEOF(UrlPattern::tryCompile(input.asPtr(), UrlPattern::CompileOptions {
      .baseUrl = input.asPtr()
    })) {
      KJ_CASE_ONEOF(str, kj::String) {}
      KJ_CASE_ONEOF(pattern, UrlPattern) {}
    }
  }
}

// The following aren't a full test of the URL pattern... they test only whether
// an input pattern compiles or fails to compile as expected. A complete test of
// each of the patterns would require also running the match tests using the
// generated regexp

KJ_TEST("URLPattern - WPT compile failed") {
#include "url-pattern-test-corpus-failures.h"
}

KJ_TEST("URLPattern - WPT compile success") {
#include "url-pattern-test-corpus-success.h"
}

}  // namespace
}  // namespace workerd::jsg::test

