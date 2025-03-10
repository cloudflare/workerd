// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "url.h"

#include <openssl/rand.h>

#include <kj/table.h>
#include <kj/test.h>

#include <regex>

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
  struct TestCase {
    kj::StringPtr input;
    kj::Maybe<kj::StringPtr> base = kj::none;
  };
  static const TestCase TESTS[] = {{"http://f:b/c"_kj, kj::Maybe("http://example.org/foo/bar"_kj)},
    {"http://f: /c"_kj, kj::Maybe("http://example.org/foo/bar"_kj)},
    {"http://f:fifty-two/c"_kj, kj::Maybe("http://example.org/foo/bar"_kj)},
    {"http://f:999999/c"_kj, kj::Maybe("http://example.org/foo/bar"_kj)},
    {"non-special://f:999999/c"_kj, kj::Maybe("http://example.org/foo/bar"_kj)},
    {"http://f: 21 / b ? d # e "_kj, kj::Maybe("http://example.org/foo/bar"_kj)},
    {"http://[1::2]:3:4"_kj, kj::Maybe("http://example.org/foo/bar"_kj)},
    {"http://2001::1"_kj, kj::Maybe("http://example.org/foo/bar"_kj)},
    {"http://2001::1]"_kj, kj::Maybe("http://example.org/foo/bar"_kj)},
    {"http://2001::1]:80"_kj, kj::Maybe("http://example.org/foo/bar"_kj)},
    {"http://[::127.0.0.1.]"_kj, kj::Maybe("http://example.org/foo/bar"_kj)},
    {"file://example:1/"_kj}, {"file://example:test/"_kj}, {"file://example%/"_kj},
    {"file://[example]/"_kj}, {"http://user:pass@/"_kj}, {"http://foo:-80/"_kj},
    {"http:/:@/www.example.com"_kj}, {"http://user@/www.example.com"_kj},
    {"http:@/www.example.com"_kj}, {"http:/@/www.example.com"_kj}, {"http://@/www.example.com"_kj},
    {"https:@/www.example.com"_kj}, {"http:a:b@/www.example.com"_kj},
    {"http:/a:b@/www.example.com"_kj}, {"http://a:b@/www.example.com"_kj},
    {"http::@/www.example.com"_kj}, {"http:@:www.example.com"_kj}, {"http:/@:www.example.com"_kj},
    {"http://@:www.example.com"_kj},
    {"http://example example.com"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://Goo%20 goo%7C|.com"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://[]"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://[:]"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://GOO 　goo.com"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://﷐zyx.com"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://%ef%b7%90zyx.com"_kj, kj::Maybe("http://other.com/"_kj)}, {"https://�"_kj},
    {"https://%EF%BF%BD"_kj}, {"http://a.b.c.xn--pokxncvks"_kj}, {"http://10.0.0.xn--pokxncvks"_kj},
    {"http://a.b.c.XN--pokxncvks"_kj}, {"http://a.b.c.Xn--pokxncvks"_kj},
    {"http://10.0.0.XN--pokxncvks"_kj}, {"http://10.0.0.xN--pokxncvks"_kj},
    {"http://％４１.com"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://%ef%bc%85%ef%bc%94%ef%bc%91.com"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://％００.com"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://%ef%bc%85%ef%bc%90%ef%bc%90.com"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://%zz%66%a.com"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://%25"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://hello%00"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://192.168.0.257"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://%3g%78%63%30%2e%30%32%35%30%2E.01"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://192.168.0.1 hello"_kj, kj::Maybe("http://other.com/"_kj)}, {"https://x x:12"_kj},
    {"http://[www.google.com]/"_kj}, {"http://[google.com]"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://[::1.2.3.4x]"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://[::1.2.3.]"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://[::1.2.]"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://[::.1.2]"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://[::1.]"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://[::.1]"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://[::%31]"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://%5B::1]"_kj, kj::Maybe("http://other.com/"_kj)}, {"i"_kj, kj::Maybe("sc:sd"_kj)},
    {"i"_kj, kj::Maybe("sc:sd/sd"_kj)}, {"../i"_kj, kj::Maybe("sc:sd"_kj)},
    {"../i"_kj, kj::Maybe("sc:sd/sd"_kj)}, {"/i"_kj, kj::Maybe("sc:sd"_kj)},
    {"/i"_kj, kj::Maybe("sc:sd/sd"_kj)}, {"?i"_kj, kj::Maybe("sc:sd"_kj)},
    {"?i"_kj, kj::Maybe("sc:sd/sd"_kj)}, {"sc://@/"_kj}, {"sc://te@s:t@/"_kj}, {"sc://:/"_kj},
    {"sc://:12/"_kj}, {"sc://a\0b/"_kj}, {"sc://a b/"_kj}, {"sc://a<b"_kj}, {"sc://a>b"_kj},
    {"sc://a[b/"_kj}, {"sc://a\\b/"_kj}, {"sc://a]b/"_kj}, {"sc://a^b"_kj}, {"sc://a|b/"_kj},
    {"http://a\0b/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj},
    {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj},
    {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj},
    {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj},
    {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj},
    {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj},
    {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj}, {"http://ab/"_kj},
    {"http://ab/"_kj}, {"http://a b/"_kj}, {"http://a%b/"_kj}, {"http://a<b"_kj},
    {"http://a>b"_kj}, {"http://a[b/"_kj}, {"http://a]b/"_kj}, {"http://a^b"_kj},
    {"http://a|b/"_kj}, {"http://ab/"_kj}, {"http://ho%00st/"_kj}, {"http://ho%01st/"_kj},
    {"http://ho%02st/"_kj}, {"http://ho%03st/"_kj}, {"http://ho%04st/"_kj}, {"http://ho%05st/"_kj},
    {"http://ho%06st/"_kj}, {"http://ho%07st/"_kj}, {"http://ho%08st/"_kj}, {"http://ho%09st/"_kj},
    {"http://ho%0Ast/"_kj}, {"http://ho%0Bst/"_kj}, {"http://ho%0Cst/"_kj}, {"http://ho%0Dst/"_kj},
    {"http://ho%0Est/"_kj}, {"http://ho%0Fst/"_kj}, {"http://ho%10st/"_kj}, {"http://ho%11st/"_kj},
    {"http://ho%12st/"_kj}, {"http://ho%13st/"_kj}, {"http://ho%14st/"_kj}, {"http://ho%15st/"_kj},
    {"http://ho%16st/"_kj}, {"http://ho%17st/"_kj}, {"http://ho%18st/"_kj}, {"http://ho%19st/"_kj},
    {"http://ho%1Ast/"_kj}, {"http://ho%1Bst/"_kj}, {"http://ho%1Cst/"_kj}, {"http://ho%1Dst/"_kj},
    {"http://ho%1Est/"_kj}, {"http://ho%1Fst/"_kj}, {"http://ho%20st/"_kj}, {"http://ho%23st/"_kj},
    {"http://ho%25st/"_kj}, {"http://ho%2Fst/"_kj}, {"http://ho%3Ast/"_kj}, {"http://ho%3Cst/"_kj},
    {"http://ho%3Est/"_kj}, {"http://ho%3Fst/"_kj}, {"http://ho%40st/"_kj}, {"http://ho%5Bst/"_kj},
    {"http://ho%5Cst/"_kj}, {"http://ho%5Dst/"_kj}, {"http://ho%7Cst/"_kj}, {"http://ho%7Fst/"_kj},
    {"ftp://example.com%80/"_kj}, {"ftp://example.com%A0/"_kj}, {"https://example.com%80/"_kj},
    {"https://example.com%A0/"_kj}, {"http:"_kj, kj::Maybe("https://example.org/foo/bar"_kj)},
    {"http://10000000000"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://4294967296"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://0xffffffff1"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://256.256.256.256"_kj, kj::Maybe("http://other.com/"_kj)},
    {"https://0x100000000/test"_kj}, {"https://256.0.0.1/test"_kj}, {"file://%43%3A"_kj},
    {"file://%43%7C"_kj}, {"file://%43|"_kj}, {"file://C%7C"_kj}, {"file://%43%7C/"_kj},
    {"https://%43%7C/"_kj}, {"asdf://%43|/"_kj}, {"\\\\\\.\\Y:"_kj}, {"\\\\\\.\\y:"_kj},
    {"http://[0:1:2:3:4:5:6:7:8]"_kj, kj::Maybe("http://example.net/"_kj)},
    {"https://[0::0::0]"_kj}, {"https://[0:.0]"_kj}, {"https://[0:0:]"_kj},
    {"https://[0:1:2:3:4:5:6:7.0.0.0.1]"_kj}, {"https://[0:1.00.0.0.0]"_kj},
    {"https://[0:1.290.0.0.0]"_kj}, {"https://[0:1.23.23]"_kj}, {"http://?"_kj}, {"http://#"_kj},
    {"http://f:4294967377/c"_kj, kj::Maybe("http://example.org/"_kj)},
    {"http://f:18446744073709551697/c"_kj, kj::Maybe("http://example.org/"_kj)},
    {"http://f:340282366920938463463374607431768211537/c"_kj, kj::Maybe("http://example.org/"_kj)},
    {"non-special://[:80/"_kj}, {"http://[::127.0.0.0.1]"_kj}, {"a"_kj}, {"a/"_kj}, {"a//"_kj},
    {"test-a-colon.html"_kj, kj::Maybe("a:"_kj)}, {"test-a-colon-b.html"_kj, kj::Maybe("a:b"_kj)},
    {"file://­/p"_kj}, {"file://%C2%AD/p"_kj}, {"file://xn--/p"_kj}, {"#"_kj}, {"?"_kj},
    {"http://1.2.3.4.5"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://1.2.3.4.5."_kj, kj::Maybe("http://other.com/"_kj)}, {"http://0..0x300/"_kj},
    {"http://0..0x300./"_kj}, {"http://256.256.256.256.256"_kj, kj::Maybe("http://other.com/"_kj)},
    {"http://256.256.256.256.256."_kj, kj::Maybe("http://other.com/"_kj)}, {"http://1.2.3.08"_kj},
    {"http://1.2.3.08."_kj}, {"http://1.2.3.09"_kj}, {"http://09.2.3.4"_kj},
    {"http://09.2.3.4."_kj}, {"http://01.2.3.4.5"_kj}, {"http://01.2.3.4.5."_kj},
    {"http://0x100.2.3.4"_kj}, {"http://0x100.2.3.4."_kj}, {"http://0x1.2.3.4.5"_kj},
    {"http://0x1.2.3.4.5."_kj}, {"http://foo.1.2.3.4"_kj}, {"http://foo.1.2.3.4."_kj},
    {"http://foo.2.3.4"_kj}, {"http://foo.2.3.4."_kj}, {"http://foo.09"_kj}, {"http://foo.09."_kj},
    {"http://foo.0x4"_kj}, {"http://foo.0x4."_kj}, {"http://0999999999999999999/"_kj},
    {"http://foo.0x"_kj}, {"http://foo.0XFfFfFfFfFfFfFfFfFfAcE123"_kj}, {"http://💩.123/"_kj},
    {"https://\0y"_kj}, {"https://￿y"_kj}, {""_kj}, {"https://­/"_kj}, {"https://%C2%AD/"_kj},
    {"https://xn--/"_kj}, {"data://:443"_kj}, {"data://test:test"_kj}, {"data://[:1]"_kj},
    {"javascript://:443"_kj}, {"javascript://test:test"_kj}, {"javascript://[:1]"_kj},
    {"mailto://:443"_kj}, {"mailto://test:test"_kj}, {"mailto://[:1]"_kj}, {"intent://:443"_kj},
    {"intent://test:test"_kj}, {"intent://[:1]"_kj}, {"urn://:443"_kj}, {"urn://test:test"_kj},
    {"urn://[:1]"_kj}, {"turn://:443"_kj}, {"turn://test:test"_kj}, {"turn://[:1]"_kj},
    {"stun://:443"_kj}, {"stun://test:test"_kj}, {"stun://[:1]"_kj}};

  for (auto& test: TESTS) {
    KJ_IF_SOME(base, test.base) {
      KJ_ASSERT(jsg::Url::tryParse(test.input, base) == kj::none);
    } else {
      KJ_ASSERT(jsg::Url::tryParse(test.input) == kj::none);
    }
  }
}

void test(kj::StringPtr input, kj::Maybe<kj::StringPtr> base, kj::StringPtr href) {
  KJ_ASSERT(Url::canParse(input, base));
  auto url = KJ_ASSERT_NONNULL(Url::tryParse(input, base));
  KJ_ASSERT(url.getHref() == href);
}

KJ_TEST("Valid Urls") {
  struct TestCase {
    kj::StringPtr input;
    kj::Maybe<kj::StringPtr> base;
    kj::StringPtr result;
  };
  static const TestCase TESTS[] = {
    {"http://example\t.\norg"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/"_kj},
    {"http://user:pass@foo:21/bar;par?b#c"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://user:pass@foo:21/bar;par?b#c"_kj},
    {"https://test:@test"_kj, kj::none, "https://test@test/"_kj},
    {"https://:@test"_kj, kj::none, "https://test/"_kj},
    {"non-special://test:@test/x"_kj, kj::none, "non-special://test@test/x"_kj},
    {"non-special://:@test/x"_kj, kj::none, "non-special://test/x"_kj},
    {"http:foo.com"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/foo/foo.com"_kj},
    {"\t   :foo.com   \n"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/foo/:foo.com"_kj},
    {" foo.com  "_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/foo/foo.com"_kj},
    {"a:\t foo.com"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "a: foo.com"_kj},
    {"http://f:21/ b ? d # e "_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://f:21/%20b%20?%20d%20#%20e"_kj},
    {"lolscheme:x x#x x"_kj, kj::none, "lolscheme:x x#x%20x"_kj},
    {"http://f:/c"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://f/c"_kj},
    {"http://f:0/c"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://f:0/c"_kj},
    {"http://f:00000000000000/c"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://f:0/c"_kj},
    {"http://f:00000000000000000000080/c"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://f/c"_kj},
    {"http://f:\n/c"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://f/c"_kj},
    {""_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/bar"_kj},
    {"  \t"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/bar"_kj},
    {":foo.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/foo/:foo.com/"_kj},
    {":foo.com\\"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/foo/:foo.com/"_kj},
    {":"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/:"_kj},
    {":a"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/:a"_kj},
    {":/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/:/"_kj},
    {":\\"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/:/"_kj},
    {":#"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/:#"_kj},
    {"#"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/bar#"_kj},
    {"#/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/bar#/"_kj},
    {"#\\"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/bar#\\"_kj},
    {"#;?"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/bar#;?"_kj},
    {"?"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/bar?"_kj},
    {"/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/"_kj},
    {":23"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/:23"_kj},
    {"/:23"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/:23"_kj},
    {"\\x"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/x"_kj},
    {"\\\\x\\hello"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://x/hello"_kj},
    {"::"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/::"_kj},
    {"::23"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/::23"_kj},
    {"foo://"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "foo://"_kj},
    {"http://a:b@c:29/d"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://a:b@c:29/d"_kj},
    {"http::@c:29"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/foo/:@c:29"_kj},
    {"http://&a:foo(b]c@d:2/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://&a:foo(b%5Dc@d:2/"_kj},
    {"http://::@c@d:2"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://:%3A%40c@d:2/"_kj},
    {"http://foo.com:b@d/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://foo.com:b@d/"_kj},
    {"http://foo.com/\\@"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://foo.com//@"_kj},
    {"http:\\\\foo.com\\"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://foo.com/"_kj},
    {"http:\\\\a\\b:c\\d@foo.com\\"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://a/b:c/d@foo.com/"_kj},
    {"foo:/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "foo:/"_kj},
    {"foo:/bar.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "foo:/bar.com/"_kj},
    {"foo://///////"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "foo://///////"_kj},
    {"foo://///////bar.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "foo://///////bar.com/"_kj},
    {"foo:////://///"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "foo:////://///"_kj},
    {"c:/foo"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "c:/foo"_kj},
    {"//foo/bar"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://foo/bar"_kj},
    {"http://foo/path;a??e#f#g"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://foo/path;a??e#f#g"_kj},
    {"http://foo/abcd?efgh?ijkl"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://foo/abcd?efgh?ijkl"_kj},
    {"http://foo/abcd#foo?bar"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://foo/abcd#foo?bar"_kj},
    {"[61:24:74]:98"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/foo/[61:24:74]:98"_kj},
    {"http:[61:27]/:foo"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/foo/[61:27]/:foo"_kj},
    {"http://[2001::1]"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://[2001::1]/"_kj},
    {"http://[::127.0.0.1]"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://[::7f00:1]/"_kj},
    {"http://[0:0:0:0:0:0:13.1.68.3]"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://[::d01:4403]/"_kj},
    {"http://[2001::1]:80"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://[2001::1]/"_kj},
    {"http:/example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/example.com/"_kj},
    {"ftp:/example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "ftp://example.com/"_kj},
    {"https:/example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "https://example.com/"_kj},
    {"madeupscheme:/example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "madeupscheme:/example.com/"_kj},
    {"file:/example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "file:///example.com/"_kj},
    {"ftps:/example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "ftps:/example.com/"_kj},
    {"gopher:/example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "gopher:/example.com/"_kj},
    {"ws:/example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "ws://example.com/"_kj},
    {"wss:/example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "wss://example.com/"_kj},
    {"data:/example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "data:/example.com/"_kj},
    {"javascript:/example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "javascript:/example.com/"_kj},
    {"mailto:/example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "mailto:/example.com/"_kj},
    {"http:example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/foo/example.com/"_kj},
    {"ftp:example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "ftp://example.com/"_kj},
    {"https:example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "https://example.com/"_kj},
    {"madeupscheme:example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "madeupscheme:example.com/"_kj},
    {"ftps:example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "ftps:example.com/"_kj},
    {"gopher:example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "gopher:example.com/"_kj},
    {"ws:example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "ws://example.com/"_kj},
    {"wss:example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "wss://example.com/"_kj},
    {"data:example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "data:example.com/"_kj},
    {"javascript:example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "javascript:example.com/"_kj},
    {"mailto:example.com/"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "mailto:example.com/"_kj},
    {"/a/b/c"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/a/b/c"_kj},
    {"/a/ /c"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/a/%20/c"_kj},
    {"/a%2fc"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/a%2fc"_kj},
    {"/a/%2f/c"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/a/%2f/c"_kj},
    {"#β"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/bar#%CE%B2"_kj},
    {"data:text/html,test#test"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "data:text/html,test#test"_kj},
    {"tel:1234567890"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "tel:1234567890"_kj},
    {"ssh://example.com/foo/bar.git"_kj, kj::Maybe("http://example.org/"_kj),
      "ssh://example.com/foo/bar.git"_kj},
    {"file:c:\\foo\\bar.html"_kj, kj::Maybe("file:///tmp/mock/path"_kj),
      "file:///c:/foo/bar.html"_kj},
    {"  File:c|////foo\\bar.html"_kj, kj::Maybe("file:///tmp/mock/path"_kj),
      "file:///c:////foo/bar.html"_kj},
    {"C|/foo/bar"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///C:/foo/bar"_kj},
    {"/C|\\foo\\bar"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///C:/foo/bar"_kj},
    {"//C|/foo/bar"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///C:/foo/bar"_kj},
    {"//server/file"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file://server/file"_kj},
    {"\\\\server\\file"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file://server/file"_kj},
    {"/\\server/file"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file://server/file"_kj},
    {"file:///foo/bar.txt"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///foo/bar.txt"_kj},
    {"file:///home/me"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///home/me"_kj},
    {"//"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///"_kj},
    {"///"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///"_kj},
    {"///test"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///test"_kj},
    {"file://test"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file://test/"_kj},
    {"file://localhost"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///"_kj},
    {"file://localhost/"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///"_kj},
    {"file://localhost/test"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///test"_kj},
    {"test"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///tmp/mock/test"_kj},
    {"file:test"_kj, kj::Maybe("file:///tmp/mock/path"_kj), "file:///tmp/mock/test"_kj},
    {"http://example.com/././foo"_kj, kj::none, "http://example.com/foo"_kj},
    {"http://example.com/./.foo"_kj, kj::none, "http://example.com/.foo"_kj},
    {"http://example.com/foo/."_kj, kj::none, "http://example.com/foo/"_kj},
    {"http://example.com/foo/./"_kj, kj::none, "http://example.com/foo/"_kj},
    {"http://example.com/foo/bar/.."_kj, kj::none, "http://example.com/foo/"_kj},
    {"http://example.com/foo/bar/../"_kj, kj::none, "http://example.com/foo/"_kj},
    {"http://example.com/foo/..bar"_kj, kj::none, "http://example.com/foo/..bar"_kj},
    {"http://example.com/foo/bar/../ton"_kj, kj::none, "http://example.com/foo/ton"_kj},
    {"http://example.com/foo/bar/../ton/../../a"_kj, kj::none, "http://example.com/a"_kj},
    {"http://example.com/foo/../../.."_kj, kj::none, "http://example.com/"_kj},
    {"http://example.com/foo/../../../ton"_kj, kj::none, "http://example.com/ton"_kj},
    {"http://example.com/foo/%2e"_kj, kj::none, "http://example.com/foo/"_kj},
    {"http://example.com/foo/%2e%2"_kj, kj::none, "http://example.com/foo/%2e%2"_kj},
    {"http://example.com/foo/%2e./%2e%2e/.%2e/%2e.bar"_kj, kj::none,
      "http://example.com/%2e.bar"_kj},
    {"http://example.com////../.."_kj, kj::none, "http://example.com//"_kj},
    {"http://example.com/foo/bar//../.."_kj, kj::none, "http://example.com/foo/"_kj},
    {"http://example.com/foo/bar//.."_kj, kj::none, "http://example.com/foo/bar/"_kj},
    {"http://example.com/foo"_kj, kj::none, "http://example.com/foo"_kj},
    {"http://example.com/%20foo"_kj, kj::none, "http://example.com/%20foo"_kj},
    {"http://example.com/foo%"_kj, kj::none, "http://example.com/foo%"_kj},
    {"http://example.com/foo%2"_kj, kj::none, "http://example.com/foo%2"_kj},
    {"http://example.com/foo%2zbar"_kj, kj::none, "http://example.com/foo%2zbar"_kj},
    {"http://example.com/foo%2Â©zbar"_kj, kj::none, "http://example.com/foo%2%C3%82%C2%A9zbar"_kj},
    {"http://example.com/foo%41%7a"_kj, kj::none, "http://example.com/foo%41%7a"_kj},
    {"http://example.com/foo\t%91"_kj, kj::none, "http://example.com/foo%C2%91%91"_kj},
    {"http://example.com/foo%00%51"_kj, kj::none, "http://example.com/foo%00%51"_kj},
    {"http://example.com/(%28:%3A%29)"_kj, kj::none, "http://example.com/(%28:%3A%29)"_kj},
    {"http://example.com/%3A%3a%3C%3c"_kj, kj::none, "http://example.com/%3A%3a%3C%3c"_kj},
    {"http://example.com/foo\tbar"_kj, kj::none, "http://example.com/foobar"_kj},
    {"http://example.com\\\\foo\\\\bar"_kj, kj::none, "http://example.com//foo//bar"_kj},
    {"http://example.com/%7Ffp3%3Eju%3Dduvgw%3Dd"_kj, kj::none,
      "http://example.com/%7Ffp3%3Eju%3Dduvgw%3Dd"_kj},
    {"http://example.com/@asdf%40"_kj, kj::none, "http://example.com/@asdf%40"_kj},
    {"http://example.com/你好你好"_kj, kj::none,
      "http://example.com/%E4%BD%A0%E5%A5%BD%E4%BD%A0%E5%A5%BD"_kj},
    {"http://example.com/‥/foo"_kj, kj::none, "http://example.com/%E2%80%A5/foo"_kj},
    {"http://example.com/﻿/foo"_kj, kj::none, "http://example.com/%EF%BB%BF/foo"_kj},
    {"http://example.com/‮/foo/‭/bar"_kj, kj::none,
      "http://example.com/%E2%80%AE/foo/%E2%80%AD/bar"_kj},
    {"http://www.google.com/foo?bar=baz#"_kj, kj::none, "http://www.google.com/foo?bar=baz#"_kj},
    {"http://www.google.com/foo?bar=baz# »"_kj, kj::none,
      "http://www.google.com/foo?bar=baz#%20%C2%BB"_kj},
    {"data:test# »"_kj, kj::none, "data:test#%20%C2%BB"_kj},
    {"http://www.google.com"_kj, kj::none, "http://www.google.com/"_kj},
    {"http://192.0x00A80001"_kj, kj::none, "http://192.168.0.1/"_kj},
    {"http://www/foo%2Ehtml"_kj, kj::none, "http://www/foo%2Ehtml"_kj},
    {"http://www/foo/%2E/html"_kj, kj::none, "http://www/foo/html"_kj},
    {"http://%25DOMAIN:foobar@foodomain.com/"_kj, kj::none,
      "http://%25DOMAIN:foobar@foodomain.com/"_kj},
    {"http:\\\\www.google.com\\foo"_kj, kj::none, "http://www.google.com/foo"_kj},
    {"http://foo:80/"_kj, kj::none, "http://foo/"_kj},
    {"http://foo:81/"_kj, kj::none, "http://foo:81/"_kj},
    {"httpa://foo:80/"_kj, kj::none, "httpa://foo:80/"_kj},
    {"https://foo:443/"_kj, kj::none, "https://foo/"_kj},
    {"https://foo:80/"_kj, kj::none, "https://foo:80/"_kj},
    {"ftp://foo:21/"_kj, kj::none, "ftp://foo/"_kj},
    {"ftp://foo:80/"_kj, kj::none, "ftp://foo:80/"_kj},
    {"gopher://foo:70/"_kj, kj::none, "gopher://foo:70/"_kj},
    {"gopher://foo:443/"_kj, kj::none, "gopher://foo:443/"_kj},
    {"ws://foo:80/"_kj, kj::none, "ws://foo/"_kj}, {"ws://foo:81/"_kj, kj::none, "ws://foo:81/"_kj},
    {"ws://foo:443/"_kj, kj::none, "ws://foo:443/"_kj},
    {"ws://foo:815/"_kj, kj::none, "ws://foo:815/"_kj},
    {"wss://foo:80/"_kj, kj::none, "wss://foo:80/"_kj},
    {"wss://foo:81/"_kj, kj::none, "wss://foo:81/"_kj},
    {"wss://foo:443/"_kj, kj::none, "wss://foo/"_kj},
    {"wss://foo:815/"_kj, kj::none, "wss://foo:815/"_kj},
    {"http:/example.com/"_kj, kj::none, "http://example.com/"_kj},
    {"ftp:/example.com/"_kj, kj::none, "ftp://example.com/"_kj},
    {"https:/example.com/"_kj, kj::none, "https://example.com/"_kj},
    {"madeupscheme:/example.com/"_kj, kj::none, "madeupscheme:/example.com/"_kj},
    {"file:/example.com/"_kj, kj::none, "file:///example.com/"_kj},
    {"ftps:/example.com/"_kj, kj::none, "ftps:/example.com/"_kj},
    {"gopher:/example.com/"_kj, kj::none, "gopher:/example.com/"_kj},
    {"ws:/example.com/"_kj, kj::none, "ws://example.com/"_kj},
    {"wss:/example.com/"_kj, kj::none, "wss://example.com/"_kj},
    {"data:/example.com/"_kj, kj::none, "data:/example.com/"_kj},
    {"javascript:/example.com/"_kj, kj::none, "javascript:/example.com/"_kj},
    {"mailto:/example.com/"_kj, kj::none, "mailto:/example.com/"_kj},
    {"http:example.com/"_kj, kj::none, "http://example.com/"_kj},
    {"ftp:example.com/"_kj, kj::none, "ftp://example.com/"_kj},
    {"https:example.com/"_kj, kj::none, "https://example.com/"_kj},
    {"madeupscheme:example.com/"_kj, kj::none, "madeupscheme:example.com/"_kj},
    {"ftps:example.com/"_kj, kj::none, "ftps:example.com/"_kj},
    {"gopher:example.com/"_kj, kj::none, "gopher:example.com/"_kj},
    {"ws:example.com/"_kj, kj::none, "ws://example.com/"_kj},
    {"wss:example.com/"_kj, kj::none, "wss://example.com/"_kj},
    {"data:example.com/"_kj, kj::none, "data:example.com/"_kj},
    {"javascript:example.com/"_kj, kj::none, "javascript:example.com/"_kj},
    {"mailto:example.com/"_kj, kj::none, "mailto:example.com/"_kj},
    {"http:@www.example.com"_kj, kj::none, "http://www.example.com/"_kj},
    {"http:/@www.example.com"_kj, kj::none, "http://www.example.com/"_kj},
    {"http://@www.example.com"_kj, kj::none, "http://www.example.com/"_kj},
    {"http:a:b@www.example.com"_kj, kj::none, "http://a:b@www.example.com/"_kj},
    {"http:/a:b@www.example.com"_kj, kj::none, "http://a:b@www.example.com/"_kj},
    {"http://a:b@www.example.com"_kj, kj::none, "http://a:b@www.example.com/"_kj},
    {"http://@pple.com"_kj, kj::none, "http://pple.com/"_kj},
    {"http::b@www.example.com"_kj, kj::none, "http://:b@www.example.com/"_kj},
    {"http:/:b@www.example.com"_kj, kj::none, "http://:b@www.example.com/"_kj},
    {"http://:b@www.example.com"_kj, kj::none, "http://:b@www.example.com/"_kj},
    {"http:a:@www.example.com"_kj, kj::none, "http://a@www.example.com/"_kj},
    {"http:/a:@www.example.com"_kj, kj::none, "http://a@www.example.com/"_kj},
    {"http://a:@www.example.com"_kj, kj::none, "http://a@www.example.com/"_kj},
    {"http://www.@pple.com"_kj, kj::none, "http://www.@pple.com/"_kj},
    {"http://:@www.example.com"_kj, kj::none, "http://www.example.com/"_kj},
    {"/"_kj, kj::Maybe("http://www.example.com/test"_kj), "http://www.example.com/"_kj},
    {"/test.txt"_kj, kj::Maybe("http://www.example.com/test"_kj),
      "http://www.example.com/test.txt"_kj},
    {"."_kj, kj::Maybe("http://www.example.com/test"_kj), "http://www.example.com/"_kj},
    {".."_kj, kj::Maybe("http://www.example.com/test"_kj), "http://www.example.com/"_kj},
    {"test.txt"_kj, kj::Maybe("http://www.example.com/test"_kj),
      "http://www.example.com/test.txt"_kj},
    {"./test.txt"_kj, kj::Maybe("http://www.example.com/test"_kj),
      "http://www.example.com/test.txt"_kj},
    {"../test.txt"_kj, kj::Maybe("http://www.example.com/test"_kj),
      "http://www.example.com/test.txt"_kj},
    {"../aaa/test.txt"_kj, kj::Maybe("http://www.example.com/test"_kj),
      "http://www.example.com/aaa/test.txt"_kj},
    {"../../test.txt"_kj, kj::Maybe("http://www.example.com/test"_kj),
      "http://www.example.com/test.txt"_kj},
    {"中/test.txt"_kj, kj::Maybe("http://www.example.com/test"_kj),
      "http://www.example.com/%E4%B8%AD/test.txt"_kj},
    {"http://www.example2.com"_kj, kj::Maybe("http://www.example.com/test"_kj),
      "http://www.example2.com/"_kj},
    {"//www.example2.com"_kj, kj::Maybe("http://www.example.com/test"_kj),
      "http://www.example2.com/"_kj},
    {"file:..."_kj, kj::Maybe("http://www.example.com/test"_kj), "file:///..."_kj},
    {"file:.."_kj, kj::Maybe("http://www.example.com/test"_kj), "file:///"_kj},
    {"file:a"_kj, kj::Maybe("http://www.example.com/test"_kj), "file:///a"_kj},
    {"http://ExAmPlE.CoM"_kj, kj::Maybe("http://other.com/"_kj), "http://example.com/"_kj},
    {"http://GOO​⁠﻿goo.com"_kj, kj::Maybe("http://other.com/"_kj), "http://googoo.com/"_kj},
    {"\0 http://example.com/ \r "_kj, kj::none, "http://example.com/"_kj},
    {"http://www.foo。bar.com"_kj, kj::Maybe("http://other.com/"_kj), "http://www.foo.bar.com/"_kj},
    {"https://x/�?�#�"_kj, kj::none, "https://x/%EF%BF%BD?%EF%BF%BD#%EF%BF%BD"_kj},
    {"http://Ｇｏ.com"_kj, kj::Maybe("http://other.com/"_kj), "http://go.com/"_kj},
    {"http://你好你好"_kj, kj::Maybe("http://other.com/"_kj), "http://xn--6qqa088eba/"_kj},
    {"https://faß.ExAmPlE/"_kj, kj::none, "https://xn--fa-hia.example/"_kj},
    {"sc://faß.ExAmPlE/"_kj, kj::none, "sc://fa%C3%9F.ExAmPlE/"_kj},
    {"http://%30%78%63%30%2e%30%32%35%30.01"_kj, kj::Maybe("http://other.com/"_kj),
      "http://192.168.0.1/"_kj},
    {"http://%30%78%63%30%2e%30%32%35%30.01%2e"_kj, kj::Maybe("http://other.com/"_kj),
      "http://192.168.0.1/"_kj},
    {"http://０Ｘｃ０．０２５０．０１"_kj, kj::Maybe("http://other.com/"_kj),
      "http://192.168.0.1/"_kj},
    {"http://./"_kj, kj::none, "http://./"_kj}, {"http://../"_kj, kj::none, "http://../"_kj},
    {"h://."_kj, kj::none, "h://."_kj},
    {"http://foo:💩@example.com/bar"_kj, kj::Maybe("http://other.com/"_kj),
      "http://foo:%F0%9F%92%A9@example.com/bar"_kj},
    {"#"_kj, kj::Maybe("test:test"_kj), "test:test#"_kj},
    {"#x"_kj, kj::Maybe("mailto:x@x.com"_kj), "mailto:x@x.com#x"_kj},
    {"#x"_kj, kj::Maybe("data:,"_kj), "data:,#x"_kj},
    {"#x"_kj, kj::Maybe("about:blank"_kj), "about:blank#x"_kj},
    {"#x:y"_kj, kj::Maybe("about:blank"_kj), "about:blank#x:y"_kj},
    {"#"_kj, kj::Maybe("test:test?test"_kj), "test:test?test#"_kj},
    {"https://@test@test@example:800/"_kj, kj::Maybe("http://doesnotmatter/"_kj),
      "https://%40test%40test@example:800/"_kj},
    {"https://@@@example"_kj, kj::Maybe("http://doesnotmatter/"_kj), "https://%40%40@example/"_kj},
    {"http://`{}:`{}@h/`{}?`{}"_kj, kj::Maybe("http://doesnotmatter/"_kj),
      "http://%60%7B%7D:%60%7B%7D@h/%60%7B%7D?`{}"_kj},
    {"http://host/?'"_kj, kj::none, "http://host/?%27"_kj},
    {"notspecial://host/?'"_kj, kj::none, "notspecial://host/?'"_kj},
    {"/some/path"_kj, kj::Maybe("http://user@example.org/smth"_kj),
      "http://user@example.org/some/path"_kj},
    {""_kj, kj::Maybe("http://user:pass@example.org:21/smth"_kj),
      "http://user:pass@example.org:21/smth"_kj},
    {"/some/path"_kj, kj::Maybe("http://user:pass@example.org:21/smth"_kj),
      "http://user:pass@example.org:21/some/path"_kj},
    {"i"_kj, kj::Maybe("sc:/pa/pa"_kj), "sc:/pa/i"_kj},
    {"i"_kj, kj::Maybe("sc://ho/pa"_kj), "sc://ho/i"_kj},
    {"i"_kj, kj::Maybe("sc:///pa/pa"_kj), "sc:///pa/i"_kj},
    {"../i"_kj, kj::Maybe("sc:/pa/pa"_kj), "sc:/i"_kj},
    {"../i"_kj, kj::Maybe("sc://ho/pa"_kj), "sc://ho/i"_kj},
    {"../i"_kj, kj::Maybe("sc:///pa/pa"_kj), "sc:///i"_kj},
    {"/i"_kj, kj::Maybe("sc:/pa/pa"_kj), "sc:/i"_kj},
    {"/i"_kj, kj::Maybe("sc://ho/pa"_kj), "sc://ho/i"_kj},
    {"/i"_kj, kj::Maybe("sc:///pa/pa"_kj), "sc:///i"_kj},
    {"?i"_kj, kj::Maybe("sc:/pa/pa"_kj), "sc:/pa/pa?i"_kj},
    {"?i"_kj, kj::Maybe("sc://ho/pa"_kj), "sc://ho/pa?i"_kj},
    {"?i"_kj, kj::Maybe("sc:///pa/pa"_kj), "sc:///pa/pa?i"_kj},
    {"#i"_kj, kj::Maybe("sc:sd"_kj), "sc:sd#i"_kj},
    {"#i"_kj, kj::Maybe("sc:sd/sd"_kj), "sc:sd/sd#i"_kj},
    {"#i"_kj, kj::Maybe("sc:/pa/pa"_kj), "sc:/pa/pa#i"_kj},
    {"#i"_kj, kj::Maybe("sc://ho/pa"_kj), "sc://ho/pa#i"_kj},
    {"#i"_kj, kj::Maybe("sc:///pa/pa"_kj), "sc:///pa/pa#i"_kj},
    {"about:/../"_kj, kj::none, "about:/"_kj}, {"data:/../"_kj, kj::none, "data:/"_kj},
    {"javascript:/../"_kj, kj::none, "javascript:/"_kj},
    {"mailto:/../"_kj, kj::none, "mailto:/"_kj},
    {"sc://ñ.test/"_kj, kj::none, "sc://%C3%B1.test/"_kj}, {"sc://%/"_kj, kj::none, "sc://%/"_kj},
    {"x"_kj, kj::Maybe("sc://ñ"_kj), "sc://%C3%B1/x"_kj}, {"sc:\\../"_kj, kj::none, "sc:\\../"_kj},
    {"sc::a@example.net"_kj, kj::none, "sc::a@example.net"_kj},
    {"wow:%NBD"_kj, kj::none, "wow:%NBD"_kj}, {"wow:%1G"_kj, kj::none, "wow:%1G"_kj},
    {"wow:￿"_kj, kj::none, "wow:%EF%BF%BF"_kj},
    {"http://example.com/�𐟾�﷐﷏﷯ﷰ￾￿?�𐟾�﷐﷏﷯ﷰ￾￿"_kj, kj::none,
      "http://example.com/%EF%BF%BD%F0%90%9F%BE%EF%BF%BD%EF%B7%90%EF%B7%8F%EF%B7%AF%EF%B7%B0%EF%BF%BE%EF%BF%BF?%EF%BF%BD%F0%90%9F%BE%EF%BF%BD%EF%B7%90%EF%B7%8F%EF%B7%AF%EF%B7%B0%EF%BF%BE%EF%BF%BF"_kj},
    {"foo://ho\tst/"_kj, kj::none, "foo://host/"_kj},
    {"foo://ho\nst/"_kj, kj::none, "foo://host/"_kj},
    {"foo://ho\rst/"_kj, kj::none, "foo://host/"_kj},
    {"http://ho\tst/"_kj, kj::none, "http://host/"_kj},
    {"http://ho\nst/"_kj, kj::none, "http://host/"_kj},
    {"http://ho\rst/"_kj, kj::none, "http://host/"_kj},
    {"http://!\"$&'()*+,-.;=_`{}~/"_kj, kj::none, "http://!\"$&'()*+,-.;=_`{}~/"_kj},
    {"sc://!\"$%&'()*+,-.;=_`{}~/"_kj, kj::none,
      "sc://%01%02%03%04%05%06%07%08%0B%0C%0E%0F%10%11%12%13%14%15%16%17%18%19%1A%1B%1C%1D%1E%1F%7F!\"$%&'()*+,-.;=_`{}~/"_kj},
    {"ftp://%e2%98%83"_kj, kj::none, "ftp://xn--n3h/"_kj},
    {"https://%e2%98%83"_kj, kj::none, "https://xn--n3h/"_kj},
    {"http://127.0.0.1:10100/relative_import.html"_kj, kj::none,
      "http://127.0.0.1:10100/relative_import.html"_kj},
    {"http://facebook.com/?foo=%7B%22abc%22"_kj, kj::none,
      "http://facebook.com/?foo=%7B%22abc%22"_kj},
    {"https://localhost:3000/jqueryui@1.2.3"_kj, kj::none,
      "https://localhost:3000/jqueryui@1.2.3"_kj},
    {"h\tt\nt\rp://h\to\ns\rt:9\t0\n0\r0/p\ta\nt\rh?q\tu\ne\rry#f\tr\na\rg"_kj, kj::none,
      "http://host:9000/path?query#frag"_kj},
    {"?a=b&c=d"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/foo/bar?a=b&c=d"_kj},
    {"??a=b&c=d"_kj, kj::Maybe("http://example.org/foo/bar"_kj),
      "http://example.org/foo/bar??a=b&c=d"_kj},
    {"http:"_kj, kj::Maybe("http://example.org/foo/bar"_kj), "http://example.org/foo/bar"_kj},
    {"sc:"_kj, kj::Maybe("https://example.org/foo/bar"_kj), "sc:"_kj},
    {"http://foo.bar/baz?qux#foobar"_kj, kj::none, "http://foo.bar/baz?qux#foo%08bar"_kj},
    {"http://foo.bar/baz?qux#foo\"bar"_kj, kj::none, "http://foo.bar/baz?qux#foo%22bar"_kj},
    {"http://foo.bar/baz?qux#foo<bar"_kj, kj::none, "http://foo.bar/baz?qux#foo%3Cbar"_kj},
    {"http://foo.bar/baz?qux#foo>bar"_kj, kj::none, "http://foo.bar/baz?qux#foo%3Ebar"_kj},
    {"http://foo.bar/baz?qux#foo`bar"_kj, kj::none, "http://foo.bar/baz?qux#foo%60bar"_kj},
    {"http://1.2.3.4/"_kj, kj::Maybe("http://other.com/"_kj), "http://1.2.3.4/"_kj},
    {"http://1.2.3.4./"_kj, kj::Maybe("http://other.com/"_kj), "http://1.2.3.4/"_kj},
    {"http://192.168.257"_kj, kj::Maybe("http://other.com/"_kj), "http://192.168.1.1/"_kj},
    {"http://192.168.257."_kj, kj::Maybe("http://other.com/"_kj), "http://192.168.1.1/"_kj},
    {"http://192.168.257.com"_kj, kj::Maybe("http://other.com/"_kj), "http://192.168.257.com/"_kj},
    {"http://256"_kj, kj::Maybe("http://other.com/"_kj), "http://0.0.1.0/"_kj},
    {"http://256.com"_kj, kj::Maybe("http://other.com/"_kj), "http://256.com/"_kj},
    {"http://999999999"_kj, kj::Maybe("http://other.com/"_kj), "http://59.154.201.255/"_kj},
    {"http://999999999."_kj, kj::Maybe("http://other.com/"_kj), "http://59.154.201.255/"_kj},
    {"http://999999999.com"_kj, kj::Maybe("http://other.com/"_kj), "http://999999999.com/"_kj},
    {"http://10000000000.com"_kj, kj::Maybe("http://other.com/"_kj), "http://10000000000.com/"_kj},
    {"http://4294967295"_kj, kj::Maybe("http://other.com/"_kj), "http://255.255.255.255/"_kj},
    {"http://0xffffffff"_kj, kj::Maybe("http://other.com/"_kj), "http://255.255.255.255/"_kj},
    {"https://0x.0x.0"_kj, kj::none, "https://0.0.0.0/"_kj},
    {"file:///C%3A/"_kj, kj::none, "file:///C%3A/"_kj},
    {"file:///C%7C/"_kj, kj::none, "file:///C%7C/"_kj},
    {"asdf://%43%7C/"_kj, kj::none, "asdf://%43%7C/"_kj},
    {"pix/submit.gif"_kj,
      kj::Maybe(
          "file:///C:/Users/Domenic/Dropbox/GitHub/tmpvar/jsdom/test/level2/html/files/anchor.html"_kj),
      "file:///C:/Users/Domenic/Dropbox/GitHub/tmpvar/jsdom/test/level2/html/files/pix/submit.gif"_kj},
    {".."_kj, kj::Maybe("file:///C:/"_kj), "file:///C:/"_kj},
    {".."_kj, kj::Maybe("file:///"_kj), "file:///"_kj},
    {"/"_kj, kj::Maybe("file:///C:/a/b"_kj), "file:///C:/"_kj},
    {"/"_kj, kj::Maybe("file://h/C:/a/b"_kj), "file://h/C:/"_kj},
    {"/"_kj, kj::Maybe("file://h/a/b"_kj), "file://h/"_kj},
    {"//d:"_kj, kj::Maybe("file:///C:/a/b"_kj), "file:///d:"_kj},
    {"//d:/.."_kj, kj::Maybe("file:///C:/a/b"_kj), "file:///d:/"_kj},
    {".."_kj, kj::Maybe("file:///ab:/"_kj), "file:///"_kj},
    {".."_kj, kj::Maybe("file:///1:/"_kj), "file:///"_kj},
    {""_kj, kj::Maybe("file:///test?test#test"_kj), "file:///test?test"_kj},
    {"file:"_kj, kj::Maybe("file:///test?test#test"_kj), "file:///test?test"_kj},
    {"?x"_kj, kj::Maybe("file:///test?test#test"_kj), "file:///test?x"_kj},
    {"file:?x"_kj, kj::Maybe("file:///test?test#test"_kj), "file:///test?x"_kj},
    {"#x"_kj, kj::Maybe("file:///test?test#test"_kj), "file:///test?test#x"_kj},
    {"file:#x"_kj, kj::Maybe("file:///test?test#test"_kj), "file:///test?test#x"_kj},
    {"file:\\\\//"_kj, kj::none, "file:////"_kj}, {"file:\\\\\\\\"_kj, kj::none, "file:////"_kj},
    {"file:\\\\\\\\?fox"_kj, kj::none, "file:////?fox"_kj},
    {"file:\\\\\\\\#guppy"_kj, kj::none, "file:////#guppy"_kj},
    {"file://spider///"_kj, kj::none, "file://spider///"_kj},
    {"file:\\\\localhost//"_kj, kj::none, "file:////"_kj},
    {"file:///localhost//cat"_kj, kj::none, "file:///localhost//cat"_kj},
    {"file://\\/localhost//cat"_kj, kj::none, "file:////localhost//cat"_kj},
    {"file://localhost//a//../..//"_kj, kj::none, "file://///"_kj},
    {"/////mouse"_kj, kj::Maybe("file:///elephant"_kj), "file://///mouse"_kj},
    {"\\//pig"_kj, kj::Maybe("file://lion/"_kj), "file:///pig"_kj},
    {"\\/localhost//pig"_kj, kj::Maybe("file://lion/"_kj), "file:////pig"_kj},
    {"//localhost//pig"_kj, kj::Maybe("file://lion/"_kj), "file:////pig"_kj},
    {"/..//localhost//pig"_kj, kj::Maybe("file://lion/"_kj), "file://lion//localhost//pig"_kj},
    {"file://"_kj, kj::Maybe("file://ape/"_kj), "file:///"_kj},
    {"/rooibos"_kj, kj::Maybe("file://tea/"_kj), "file://tea/rooibos"_kj},
    {"/?chai"_kj, kj::Maybe("file://tea/"_kj), "file://tea/?chai"_kj},
    {"C|"_kj, kj::Maybe("file://host/dir/file"_kj), "file://host/C:"_kj},
    {"C|"_kj, kj::Maybe("file://host/D:/dir1/dir2/file"_kj), "file://host/C:"_kj},
    {"C|#"_kj, kj::Maybe("file://host/dir/file"_kj), "file://host/C:#"_kj},
    {"C|?"_kj, kj::Maybe("file://host/dir/file"_kj), "file://host/C:?"_kj},
    {"C|/"_kj, kj::Maybe("file://host/dir/file"_kj), "file://host/C:/"_kj},
    {"C|\n/"_kj, kj::Maybe("file://host/dir/file"_kj), "file://host/C:/"_kj},
    {"C|\\"_kj, kj::Maybe("file://host/dir/file"_kj), "file://host/C:/"_kj},
    {"C"_kj, kj::Maybe("file://host/dir/file"_kj), "file://host/dir/C"_kj},
    {"C|a"_kj, kj::Maybe("file://host/dir/file"_kj), "file://host/dir/C|a"_kj},
    {"/c:/foo/bar"_kj, kj::Maybe("file:///c:/baz/qux"_kj), "file:///c:/foo/bar"_kj},
    {"/c|/foo/bar"_kj, kj::Maybe("file:///c:/baz/qux"_kj), "file:///c:/foo/bar"_kj},
    {"file:\\c:\\foo\\bar"_kj, kj::Maybe("file:///c:/baz/qux"_kj), "file:///c:/foo/bar"_kj},
    {"/c:/foo/bar"_kj, kj::Maybe("file://host/path"_kj), "file://host/c:/foo/bar"_kj},
    {"file://example.net/C:/"_kj, kj::none, "file://example.net/C:/"_kj},
    {"file://1.2.3.4/C:/"_kj, kj::none, "file://1.2.3.4/C:/"_kj},
    {"file://[1::8]/C:/"_kj, kj::none, "file://[1::8]/C:/"_kj},
    {"C|/"_kj, kj::Maybe("file://host/"_kj), "file://host/C:/"_kj},
    {"/C:/"_kj, kj::Maybe("file://host/"_kj), "file://host/C:/"_kj},
    {"file:C:/"_kj, kj::Maybe("file://host/"_kj), "file://host/C:/"_kj},
    {"file:/C:/"_kj, kj::Maybe("file://host/"_kj), "file://host/C:/"_kj},
    {"//C:/"_kj, kj::Maybe("file://host/"_kj), "file:///C:/"_kj},
    {"file://C:/"_kj, kj::Maybe("file://host/"_kj), "file:///C:/"_kj},
    {"///C:/"_kj, kj::Maybe("file://host/"_kj), "file:///C:/"_kj},
    {"file:///C:/"_kj, kj::Maybe("file://host/"_kj), "file:///C:/"_kj},
    {"file:/C|/"_kj, kj::none, "file:///C:/"_kj}, {"file://C|/"_kj, kj::none, "file:///C:/"_kj},
    {"file:"_kj, kj::none, "file:///"_kj}, {"file:?q=v"_kj, kj::none, "file:///?q=v"_kj},
    {"file:#frag"_kj, kj::none, "file:///#frag"_kj}, {"file:///Y:"_kj, kj::none, "file:///Y:"_kj},
    {"file:///Y:/"_kj, kj::none, "file:///Y:/"_kj}, {"file:///./Y"_kj, kj::none, "file:///Y"_kj},
    {"file:///./Y:"_kj, kj::none, "file:///Y:"_kj}, {"file:///y:"_kj, kj::none, "file:///y:"_kj},
    {"file:///y:/"_kj, kj::none, "file:///y:/"_kj}, {"file:///./y"_kj, kj::none, "file:///y"_kj},
    {"file:///./y:"_kj, kj::none, "file:///y:"_kj},
    {"file://localhost//a//../..//foo"_kj, kj::none, "file://///foo"_kj},
    {"file://localhost////foo"_kj, kj::none, "file://////foo"_kj},
    {"file:////foo"_kj, kj::none, "file:////foo"_kj},
    {"file:///one/two"_kj, kj::Maybe("file:///"_kj), "file:///one/two"_kj},
    {"file:////one/two"_kj, kj::Maybe("file:///"_kj), "file:////one/two"_kj},
    {"//one/two"_kj, kj::Maybe("file:///"_kj), "file://one/two"_kj},
    {"///one/two"_kj, kj::Maybe("file:///"_kj), "file:///one/two"_kj},
    {"////one/two"_kj, kj::Maybe("file:///"_kj), "file:////one/two"_kj},
    {"file:///.//"_kj, kj::Maybe("file:////"_kj), "file:////"_kj},
    {"file:.//p"_kj, kj::none, "file:////p"_kj}, {"file:/.//p"_kj, kj::none, "file:////p"_kj},
    {"http://[1:0::]"_kj, kj::Maybe("http://example.net/"_kj), "http://[1::]/"_kj},
    {"sc://ñ"_kj, kj::none, "sc://%C3%B1"_kj}, {"sc://ñ?x"_kj, kj::none, "sc://%C3%B1?x"_kj},
    {"sc://ñ#x"_kj, kj::none, "sc://%C3%B1#x"_kj},
    {"#x"_kj, kj::Maybe("sc://ñ"_kj), "sc://%C3%B1#x"_kj},
    {"?x"_kj, kj::Maybe("sc://ñ"_kj), "sc://%C3%B1?x"_kj}, {"sc://?"_kj, kj::none, "sc://?"_kj},
    {"sc://#"_kj, kj::none, "sc://#"_kj}, {"///"_kj, kj::Maybe("sc://x/"_kj), "sc:///"_kj},
    {"////"_kj, kj::Maybe("sc://x/"_kj), "sc:////"_kj},
    {"////x/"_kj, kj::Maybe("sc://x/"_kj), "sc:////x/"_kj},
    {"tftp://foobar.com/someconfig;mode=netascii"_kj, kj::none,
      "tftp://foobar.com/someconfig;mode=netascii"_kj},
    {"telnet://user:pass@foobar.com:23/"_kj, kj::none, "telnet://user:pass@foobar.com:23/"_kj},
    {"ut2004://10.10.10.10:7777/Index.ut2"_kj, kj::none, "ut2004://10.10.10.10:7777/Index.ut2"_kj},
    {"redis://foo:bar@somehost:6379/0?baz=bam&qux=baz"_kj, kj::none,
      "redis://foo:bar@somehost:6379/0?baz=bam&qux=baz"_kj},
    {"rsync://foo@host:911/sup"_kj, kj::none, "rsync://foo@host:911/sup"_kj},
    {"git://github.com/foo/bar.git"_kj, kj::none, "git://github.com/foo/bar.git"_kj},
    {"irc://myserver.com:6999/channel?passwd"_kj, kj::none,
      "irc://myserver.com:6999/channel?passwd"_kj},
    {"dns://fw.example.org:9999/foo.bar.org?type=TXT"_kj, kj::none,
      "dns://fw.example.org:9999/foo.bar.org?type=TXT"_kj},
    {"ldap://localhost:389/ou=People,o=JNDITutorial"_kj, kj::none,
      "ldap://localhost:389/ou=People,o=JNDITutorial"_kj},
    {"git+https://github.com/foo/bar"_kj, kj::none, "git+https://github.com/foo/bar"_kj},
    {"urn:ietf:rfc:2648"_kj, kj::none, "urn:ietf:rfc:2648"_kj},
    {"tag:joe@example.org,2001:foo/bar"_kj, kj::none, "tag:joe@example.org,2001:foo/bar"_kj},
    {"non-spec:/.//"_kj, kj::none, "non-spec:/.//"_kj},
    {"non-spec:/..//"_kj, kj::none, "non-spec:/.//"_kj},
    {"non-spec:/a/..//"_kj, kj::none, "non-spec:/.//"_kj},
    {"non-spec:/.//path"_kj, kj::none, "non-spec:/.//path"_kj},
    {"non-spec:/..//path"_kj, kj::none, "non-spec:/.//path"_kj},
    {"non-spec:/a/..//path"_kj, kj::none, "non-spec:/.//path"_kj},
    {"/.//path"_kj, kj::Maybe("non-spec:/p"_kj), "non-spec:/.//path"_kj},
    {"/..//path"_kj, kj::Maybe("non-spec:/p"_kj), "non-spec:/.//path"_kj},
    {"..//path"_kj, kj::Maybe("non-spec:/p"_kj), "non-spec:/.//path"_kj},
    {"a/..//path"_kj, kj::Maybe("non-spec:/p"_kj), "non-spec:/.//path"_kj},
    {""_kj, kj::Maybe("non-spec:/..//p"_kj), "non-spec:/.//p"_kj},
    {"path"_kj, kj::Maybe("non-spec:/..//p"_kj), "non-spec:/.//path"_kj},
    {"../path"_kj, kj::Maybe("non-spec:/.//p"_kj), "non-spec:/path"_kj},
    {"non-special://%E2%80%A0/"_kj, kj::none, "non-special://%E2%80%A0/"_kj},
    {"non-special://H%4fSt/path"_kj, kj::none, "non-special://H%4fSt/path"_kj},
    {"non-special://[1:2:0:0:5:0:0:0]/"_kj, kj::none, "non-special://[1:2:0:0:5::]/"_kj},
    {"non-special://[1:2:0:0:0:0:0:3]/"_kj, kj::none, "non-special://[1:2::3]/"_kj},
    {"non-special://[1:2::3]:80/"_kj, kj::none, "non-special://[1:2::3]:80/"_kj},
    {"blob:https://example.com:443/"_kj, kj::none, "blob:https://example.com:443/"_kj},
    {"blob:http://example.org:88/"_kj, kj::none, "blob:http://example.org:88/"_kj},
    {"blob:d3958f5c-0777-0845-9dcf-2cb28783acaf"_kj, kj::none,
      "blob:d3958f5c-0777-0845-9dcf-2cb28783acaf"_kj},
    {"blob:"_kj, kj::none, "blob:"_kj}, {"blob:blob:"_kj, kj::none, "blob:blob:"_kj},
    {"blob:blob:https://example.org/"_kj, kj::none, "blob:blob:https://example.org/"_kj},
    {"blob:about:blank"_kj, kj::none, "blob:about:blank"_kj},
    {"blob:file://host/path"_kj, kj::none, "blob:file://host/path"_kj},
    {"blob:ftp://host/path"_kj, kj::none, "blob:ftp://host/path"_kj},
    {"blob:ws://example.org/"_kj, kj::none, "blob:ws://example.org/"_kj},
    {"blob:wss://example.org/"_kj, kj::none, "blob:wss://example.org/"_kj},
    {"blob:http%3a//example.org/"_kj, kj::none, "blob:http%3a//example.org/"_kj},
    {"http://0x7f.0.0.0x7g"_kj, kj::none, "http://0x7f.0.0.0x7g/"_kj},
    {"http://0X7F.0.0.0X7G"_kj, kj::none, "http://0x7f.0.0.0x7g/"_kj},
    {"http://[0:1:0:1:0:1:0:1]"_kj, kj::none, "http://[0:1:0:1:0:1:0:1]/"_kj},
    {"http://[1:0:1:0:1:0:1:0]"_kj, kj::none, "http://[1:0:1:0:1:0:1:0]/"_kj},
    {"http://example.org/test?\""_kj, kj::none, "http://example.org/test?%22"_kj},
    {"http://example.org/test?#"_kj, kj::none, "http://example.org/test?#"_kj},
    {"http://example.org/test?<"_kj, kj::none, "http://example.org/test?%3C"_kj},
    {"http://example.org/test?>"_kj, kj::none, "http://example.org/test?%3E"_kj},
    {"http://example.org/test?⌣"_kj, kj::none, "http://example.org/test?%E2%8C%A3"_kj},
    {"http://example.org/test?%23%23"_kj, kj::none, "http://example.org/test?%23%23"_kj},
    {"http://example.org/test?%GH"_kj, kj::none, "http://example.org/test?%GH"_kj},
    {"http://example.org/test?a#%EF"_kj, kj::none, "http://example.org/test?a#%EF"_kj},
    {"http://example.org/test?a#%GH"_kj, kj::none, "http://example.org/test?a#%GH"_kj},
    {"test-a-colon-slash.html"_kj, kj::Maybe("a:/"_kj), "a:/test-a-colon-slash.html"_kj},
    {"test-a-colon-slash-slash.html"_kj, kj::Maybe("a://"_kj),
      "a:///test-a-colon-slash-slash.html"_kj},
    {"test-a-colon-slash-b.html"_kj, kj::Maybe("a:/b"_kj), "a:/test-a-colon-slash-b.html"_kj},
    {"test-a-colon-slash-slash-b.html"_kj, kj::Maybe("a://b"_kj),
      "a://b/test-a-colon-slash-slash-b.html"_kj},
    {"http://example.org/test?a#b\0c"_kj, kj::none, "http://example.org/test?a#b%00c"_kj},
    {"non-spec://example.org/test?a#b\0c"_kj, kj::none, "non-spec://example.org/test?a#b%00c"_kj},
    {"non-spec:/test?a#b\0c"_kj, kj::none, "non-spec:/test?a#b%00c"_kj},
    {"10.0.0.7:8080/foo.html"_kj, kj::Maybe("file:///some/dir/bar.html"_kj),
      "file:///some/dir/10.0.0.7:8080/foo.html"_kj},
    {"a!@$*=/foo.html"_kj, kj::Maybe("file:///some/dir/bar.html"_kj),
      "file:///some/dir/a!@$*=/foo.html"_kj},
    {"a1234567890-+.:foo/bar"_kj, kj::Maybe("http://example.com/dir/file"_kj),
      "a1234567890-+.:foo/bar"_kj},
    {"file://a­b/p"_kj, kj::none, "file://ab/p"_kj},
    {"file://a%C2%ADb/p"_kj, kj::none, "file://ab/p"_kj},
    {"file://loC𝐀𝐋𝐇𝐨𝐬𝐭/usr/bin"_kj, kj::none, "file:///usr/bin"_kj},
    {"#link"_kj, kj::Maybe("https://example.org/##link"_kj), "https://example.org/#link"_kj},
    {"non-special:cannot-be-a-base-url-\0~"_kj, kj::none,
      "non-special:cannot-be-a-base-url-%00%01%1F%1E~%7F%C2%80"_kj},
    {"https://www.example.com/path{path.html?query'=query#fragment<fragment"_kj, kj::none,
      "https://www.example.com/path%7B%7Fpath.html?query%27%7F=query#fragment%3C%7Ffragment"_kj},
    {"https://user:pass[@foo/bar"_kj, kj::Maybe("http://example.org"_kj),
      "https://user:pass%5B%7F@foo/bar"_kj},
    {"foo:// !\"$%&'()*+,-.;<=>@[\\]^_`{|}~@host/"_kj, kj::none,
      "foo://%20!%22$%&'()*+,-.%3B%3C%3D%3E%40%5B%5C%5D%5E_%60%7B%7C%7D~@host/"_kj},
    {"wss:// !\"$%&'()*+,-.;<=>@[]^_`{|}~@host/"_kj, kj::none,
      "wss://%20!%22$%&'()*+,-.%3B%3C%3D%3E%40%5B%5D%5E_%60%7B%7C%7D~@host/"_kj},
    {"foo://joe: !\"$%&'()*+,-.:;<=>@[\\]^_`{|}~@host/"_kj, kj::none,
      "foo://joe:%20!%22$%&'()*+,-.%3A%3B%3C%3D%3E%40%5B%5C%5D%5E_%60%7B%7C%7D~@host/"_kj},
    {"wss://joe: !\"$%&'()*+,-.:;<=>@[]^_`{|}~@host/"_kj, kj::none,
      "wss://joe:%20!%22$%&'()*+,-.%3A%3B%3C%3D%3E%40%5B%5D%5E_%60%7B%7C%7D~@host/"_kj},
    {"foo://!\"$%&'()*+,-.;=_`{}~/"_kj, kj::none, "foo://!\"$%&'()*+,-.;=_`{}~/"_kj},
    {"wss://!\"$&'()*+,-.;=_`{}~/"_kj, kj::none, "wss://!\"$&'()*+,-.;=_`{}~/"_kj},
    {"foo://host/dir/? !\"$%&'()*+,-./:;<=>?@[\\]^_`{|}~"_kj, kj::none,
      "foo://host/dir/?%20!%22$%&'()*+,-./:;%3C=%3E?@[\\]^_`{|}~"_kj},
    {"wss://host/dir/? !\"$%&'()*+,-./:;<=>?@[\\]^_`{|}~"_kj, kj::none,
      "wss://host/dir/?%20!%22$%&%27()*+,-./:;%3C=%3E?@[\\]^_`{|}~"_kj},
    {"foo://host/dir/# !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"_kj, kj::none,
      "foo://host/dir/#%20!%22#$%&'()*+,-./:;%3C=%3E?@[\\]^_%60{|}~"_kj},
    {"wss://host/dir/# !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"_kj, kj::none,
      "wss://host/dir/#%20!%22#$%&'()*+,-./:;%3C=%3E?@[\\]^_%60{|}~"_kj},
    {"abc:rootless"_kj, kj::Maybe("abc://host/path"_kj), "abc:rootless"_kj},
    {"abc:rootless"_kj, kj::Maybe("abc:/path"_kj), "abc:rootless"_kj},
    {"abc:rootless"_kj, kj::Maybe("abc:path"_kj), "abc:rootless"_kj},
    {"abc:/rooted"_kj, kj::Maybe("abc://host/path"_kj), "abc:/rooted"_kj},
    {"http://foo.09.."_kj, kj::none, "http://foo.09../"_kj},
    {"https://x/\0y"_kj, kj::none, "https://x/%00y"_kj},
    {"https://x/?\0y"_kj, kj::none, "https://x/?%00y"_kj},
    {"https://x/?#\0y"_kj, kj::none, "https://x/?#%00y"_kj},
    {"https://x/￿y"_kj, kj::none, "https://x/%EF%BF%BFy"_kj},
    {"https://x/?￿y"_kj, kj::none, "https://x/?%EF%BF%BFy"_kj},
    {"https://x/?#￿y"_kj, kj::none, "https://x/?#%EF%BF%BFy"_kj},
    {"non-special:\0y"_kj, kj::none, "non-special:%00y"_kj},
    {"non-special:x/\0y"_kj, kj::none, "non-special:x/%00y"_kj},
    {"non-special:x/?\0y"_kj, kj::none, "non-special:x/?%00y"_kj},
    {"non-special:x/?#\0y"_kj, kj::none, "non-special:x/?#%00y"_kj},
    {"non-special:￿y"_kj, kj::none, "non-special:%EF%BF%BFy"_kj},
    {"non-special:x/￿y"_kj, kj::none, "non-special:x/%EF%BF%BFy"_kj},
    {"non-special:x/?￿y"_kj, kj::none, "non-special:x/?%EF%BF%BFy"_kj},
    {"non-special:x/?#￿y"_kj, kj::none, "non-special:x/?#%EF%BF%BFy"_kj},
    {"https://example.com/\"quoted\""_kj, kj::none, "https://example.com/%22quoted%22"_kj},
    {"https://a%C2%ADb/"_kj, kj::none, "https://ab/"_kj},
    {"data://example.com:8080/pathname?search#hash"_kj, kj::none,
      "data://example.com:8080/pathname?search#hash"_kj},
    {"data:///test"_kj, kj::none, "data:///test"_kj},
    {"data://test/a/../b"_kj, kj::none, "data://test/b"_kj},
    {"javascript://example.com:8080/pathname?search#hash"_kj, kj::none,
      "javascript://example.com:8080/pathname?search#hash"_kj},
    {"javascript:///test"_kj, kj::none, "javascript:///test"_kj},
    {"javascript://test/a/../b"_kj, kj::none, "javascript://test/b"_kj},
    {"mailto://example.com:8080/pathname?search#hash"_kj, kj::none,
      "mailto://example.com:8080/pathname?search#hash"_kj},
    {"mailto:///test"_kj, kj::none, "mailto:///test"_kj},
    {"mailto://test/a/../b"_kj, kj::none, "mailto://test/b"_kj},
    {"intent://example.com:8080/pathname?search#hash"_kj, kj::none,
      "intent://example.com:8080/pathname?search#hash"_kj},
    {"intent:///test"_kj, kj::none, "intent:///test"_kj},
    {"intent://test/a/../b"_kj, kj::none, "intent://test/b"_kj},
    {"urn://example.com:8080/pathname?search#hash"_kj, kj::none,
      "urn://example.com:8080/pathname?search#hash"_kj},
    {"urn:///test"_kj, kj::none, "urn:///test"_kj},
    {"urn://test/a/../b"_kj, kj::none, "urn://test/b"_kj},
    {"turn://example.com:8080/pathname?search#hash"_kj, kj::none,
      "turn://example.com:8080/pathname?search#hash"_kj},
    {"turn:///test"_kj, kj::none, "turn:///test"_kj},
    {"turn://test/a/../b"_kj, kj::none, "turn://test/b"_kj},
    {"stun://example.com:8080/pathname?search#hash"_kj, kj::none,
      "stun://example.com:8080/pathname?search#hash"_kj},
    {"stun:///test"_kj, kj::none, "stun:///test"_kj},
    {"stun://test/a/../b"_kj, kj::none, "stun://test/b"_kj}, {"w://x:0"_kj, kj::none, "w://x:0"_kj},
    {"west://x:0"_kj, kj::none, "west://x:0"_kj}};

  for (auto& testCase: TESTS) {
    test(testCase.input, testCase.base, testCase.result);
  }
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

KJ_TEST("Minimal URL Parse") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse 2") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org/"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Username") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://abc@example.org/"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == "abc"_kj);
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Username and Password") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://abc:xyz@example.org/"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == "abc"_kj);
  KJ_ASSERT(url.getPassword() == "xyz"_kj);
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Password, no Username") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://:xyz@example.org/"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == "xyz"_kj);
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Port (non-default)") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org:123/"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org:123"_kj);
  KJ_ASSERT(url.getHostname() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == "123"_kj);
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Port (default)") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org:443/"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Port delimiter with no port)") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org:/"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - One path segment") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org/abc"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/abc"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Leading single dot segment") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org/./abc"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/abc"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Multiple single dot segment") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org/././././abc"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/abc"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Leading double dot segment") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org/../abc"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/abc"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Leading mixed dot segment") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org/../.././.././abc"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/abc"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Three path segments") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org/a/b/c"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/a/b/c"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Three path segments with double dot") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org/a/b/../c"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/a/c"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Three path segments with single dot") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org/a/b/./c"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/a/b/c"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Query present but empty") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org?"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Query minimal") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org?123"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == "?123"_kj);
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Query minimal after missing port") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org:?123"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == "?123"_kj);
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Query minimal after missing port and empty path") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org:/?123"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == "?123"_kj);
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Fragment present but empty") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org#"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == kj::String());
}

KJ_TEST("Minimal URL Parse - Fragment minimal") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org#123"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == "#123"_kj);
}

KJ_TEST("Minimal URL Parse - Fragment minimal") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org?#123"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == kj::String());
  KJ_ASSERT(url.getHash() == "#123"_kj);
}

KJ_TEST("Minimal URL Parse - Fragment minimal") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://example.org?abc#123"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == kj::String());
  KJ_ASSERT(url.getPassword() == kj::String());
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == kj::String());
  KJ_ASSERT(url.getPathname() == "/"_kj);
  KJ_ASSERT(url.getSearch() == "?abc"_kj);
  KJ_ASSERT(url.getHash() == "#123"_kj);
}

KJ_TEST("Minimal URL Parse - All together") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://abc:xyz@example.org:123/a/b/c?abc#123"_kj));

  KJ_ASSERT(url.getProtocol() == "https:"_kj);
  KJ_ASSERT(url.getUsername() == "abc"_kj);
  KJ_ASSERT(url.getPassword() == "xyz"_kj);
  KJ_ASSERT(url.getHost() == "example.org:123"_kj);
  KJ_ASSERT(url.getHostname() == "example.org"_kj);
  KJ_ASSERT(url.getPort() == "123"_kj);
  KJ_ASSERT(url.getPathname() == "/a/b/c"_kj);
  KJ_ASSERT(url.getSearch() == "?abc"_kj);
  KJ_ASSERT(url.getHash() == "#123"_kj);
}

KJ_TEST("Minimal URL Parse - Not special (data URL)") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("data:something"_kj));

  KJ_ASSERT(url.getProtocol() == "data:"_kj);
  KJ_ASSERT(url.getPathname() == "something"_kj);
}

KJ_TEST("Minimal URL Parse - unknown scheme") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("com.tapbots.Ivory.219:/request_token?code=8"_kj));

  KJ_ASSERT(url.getProtocol() == "com.tapbots.ivory.219:"_kj);
  KJ_ASSERT(url.getPathname() == "/request_token"_kj);
  KJ_ASSERT(url.getSearch() == "?code=8"_kj);
}

KJ_TEST("Special scheme URLS") {
  kj::String tests[] = {
    kj::str("http://example.org"),
    kj::str("https://example.org"),
    kj::str("ftp://example.org"),
    kj::str("ws://example.org"),
    kj::str("wss://example.org"),
    kj::str("file:///example"),
  };

  for (auto n = 0; n < kj::size(tests); n++) {
    KJ_ASSERT_NONNULL(Url::tryParse(tests[n].asPtr()));
  }
}

KJ_TEST("Trim leading and trailing control/space") {
  auto input = kj::str(" \0\1 http://example.org \2\3 "_kj);
  auto url = KJ_ASSERT_NONNULL(Url::tryParse(input.asPtr()));
  KJ_ASSERT(url.getProtocol() == "http:"_kj);
  KJ_ASSERT(url.getHost() == "example.org"_kj);
  KJ_ASSERT(url.getPathname() == "/"_kj);
}

KJ_TEST("Percent encoding in username/password") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://%66oo:%66oo@example.com/"_kj));
  KJ_ASSERT(url.getUsername() == "%66oo"_kj);
  KJ_ASSERT(url.getPassword() == "%66oo"_kj);
}

KJ_TEST("Percent encoding in hostname") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://%66oo"_kj));
  KJ_ASSERT(url.getHost() == "foo"_kj);
}

KJ_TEST("Percent encoding in hostname") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://%66oo"_kj));
  KJ_ASSERT(url.getHost() == "foo"_kj);
}

KJ_TEST("Percent encoding in pathname") {
  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://example.org/%2e/%31%32%ZZ"_kj));
    KJ_ASSERT(url.getPathname() == "/%31%32%ZZ"_kj);
    // The %2e is properly detected as a single dot segment.
    // The invalid percent encoded %ZZ is ignored.
  }
  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://example.org/%2e/%31%32%ZZ/%2E"_kj));
    KJ_ASSERT(url.getPathname() == "/%31%32%ZZ/"_kj);
  }
}

KJ_TEST("Percent encoding in query") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://example.org?/%2e/%31%32%ZZ"_kj));
  KJ_ASSERT(url.getSearch() == "?/%2e/%31%32%ZZ"_kj);
  // The invalid percent encoded %ZZ is ignored.
}

KJ_TEST("Percent encoding in fragment") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://example.org#/%2e/%31%32%ZZ"_kj));
  KJ_ASSERT(url.getHash() == "#/%2e/%31%32%ZZ"_kj);
  // The invalid percent encoded %ZZ is ignored.
}

KJ_TEST("Percent encoding of non-ascii characters in path, query, fragment") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://example.org/café?café#café"_kj));
  KJ_ASSERT(url.getPathname() == "/caf%C3%A9"_kj);
  KJ_ASSERT(url.getSearch() == "?caf%C3%A9"_kj);
  KJ_ASSERT(url.getHash() == "#caf%C3%A9"_kj);
}

KJ_TEST("IDNA-conversion non-ascii characters in hostname") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://café.com"_kj));
  KJ_ASSERT(url.getHost() == "xn--caf-dma.com"_kj);
}

KJ_TEST("IPv4 in hostname") {
  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://123.210.123.121"_kj));
    KJ_ASSERT(url.getHost() == "123.210.123.121"_kj);
  }

  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://2077391737"_kj));
    KJ_ASSERT(url.getHost() == "123.210.123.121"_kj);
  }

  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://1.1"_kj));
    KJ_ASSERT(url.getHost() == "1.0.0.1"_kj);
  }

  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://0x1.0x1"_kj));
    KJ_ASSERT(url.getHost() == "1.0.0.1"_kj);
  }

  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://01.0x1"_kj));
    KJ_ASSERT(url.getHost() == "1.0.0.1"_kj);
  }

  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://0x1000001"_kj));
    KJ_ASSERT(url.getHost() == "1.0.0.1"_kj);
  }

  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://0100000001"_kj));
    KJ_ASSERT(url.getHost() == "1.0.0.1"_kj);
  }

  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://192.168.1"_kj));
    KJ_ASSERT(url.getHost() == "192.168.0.1"_kj);
  }

  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://192.0xa80001"_kj));
    KJ_ASSERT(url.getHost() == "192.168.0.1"_kj);
  }

  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://192.11010049"_kj));
    KJ_ASSERT(url.getHost() == "192.168.0.1"_kj);
  }

  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://0300.11010049"_kj));
    KJ_ASSERT(url.getHost() == "192.168.0.1"_kj);
  }

  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://0300.0xa80001"_kj));
    KJ_ASSERT(url.getHost() == "192.168.0.1"_kj);
  }

  {
    // Yes, this is a valid IPv4 address also.
    // You might be asking yourself, why would anyone do this?
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("http://0xc0.11010049"_kj));
    KJ_ASSERT(url.getHost() == "192.168.0.1"_kj);
  }

  {
    KJ_ASSERT(Url::tryParse("https://999.999.999.999"_kj) == kj::none);
    KJ_ASSERT(Url::tryParse("https://123.999.999.999"_kj) == kj::none);
    KJ_ASSERT(Url::tryParse("https://123.123.999.999"_kj) == kj::none);
    KJ_ASSERT(Url::tryParse("https://123.123.123.999"_kj) == kj::none);
    KJ_ASSERT(Url::tryParse("https://4294967296"_kj) == kj::none);
  }
}

KJ_TEST("IPv6 in hostname") {
  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://[1:1:1:1:1:1:1:1]"_kj));
    KJ_ASSERT(url.getHost() == "[1:1:1:1:1:1:1:1]"_kj);
  }

  {
    // Compressed segments work
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://[1::1]"_kj));
    KJ_ASSERT(url.getHost() == "[1::1]"_kj);
  }

  {
    // Compressed segments work
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://[::]"_kj));
    KJ_ASSERT(url.getHost() == "[::]"_kj);
  }

  {
    // Normalized form is shortest, lowercase serialization.
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://[11:AF:0:0:0::0001]"_kj));
    KJ_ASSERT(url.getHost() == "[11:af::1]"_kj);
  }

  {
    // IPv4-in-IPv6 syntax is supported
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("https://[2001:db8:122:344::192.0.2.33]"_kj));
    KJ_ASSERT(url.getHost() == "[2001:db8:122:344::c000:221]"_kj);
  }

  KJ_ASSERT(Url::tryParse("https://[zz::top]"_kj) == kj::none);
}

KJ_TEST("javascript: URLS") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("jAvAsCrIpT: alert('boo'); "_kj));
  KJ_ASSERT(url.getProtocol() == "javascript:"_kj);
  KJ_ASSERT(url.getPathname() == " alert('boo');"_kj);
}

KJ_TEST("data: URLS") {
  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("data:,Hello%2C%20World%21"_kj));
    KJ_ASSERT(url.getProtocol() == "data:"_kj);
    KJ_ASSERT(url.getPathname() == ",Hello%2C%20World%21"_kj);
  }
  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("data:text/plain;base64,SGVsbG8sIFdvcmxkIQ=="_kj));
    KJ_ASSERT(url.getProtocol() == "data:"_kj);
    KJ_ASSERT(url.getPathname() == "text/plain;base64,SGVsbG8sIFdvcmxkIQ=="_kj);
  }
  {
    auto url = KJ_ASSERT_NONNULL(
        Url::tryParse("data:text/html,%3Ch1%3EHello%2C%20World%21%3C%2Fh1%3E"_kj));
    KJ_ASSERT(url.getProtocol() == "data:"_kj);
    KJ_ASSERT(url.getPathname() == "text/html,%3Ch1%3EHello%2C%20World%21%3C%2Fh1%3E"_kj);
  }
  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("data:text/html,<script>alert('hi');</script>"_kj));
    KJ_ASSERT(url.getProtocol() == "data:"_kj);
    KJ_ASSERT(url.getPathname() == "text/html,<script>alert('hi');</script>"_kj);
  }
}

KJ_TEST("blob: URLS") {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("blob:https://example.org"_kj));
  KJ_ASSERT(url.getProtocol() == "blob:"_kj);
  KJ_ASSERT(url.getPathname() == "https://example.org"_kj);
}

KJ_TEST("Relative URLs") {
  {
    auto url = KJ_ASSERT_NONNULL(
        Url::tryParse(kj::String(), "https://abc:def@example.org:81/a/b/c?query#fragment"_kj));
    KJ_ASSERT(url.getProtocol() == "https:"_kj);
    KJ_ASSERT(url.getUsername() == "abc"_kj);
    KJ_ASSERT(url.getPassword() == "def"_kj);
    KJ_ASSERT(url.getHost() == "example.org:81"_kj);
    KJ_ASSERT(url.getPathname() == "/a/b/c"_kj);
    KJ_ASSERT(url.getSearch() == "?query"_kj);
    KJ_ASSERT(url.getHash() == kj::String());
  }

  {
    auto url = KJ_ASSERT_NONNULL(
        Url::tryParse("/xyz"_kj, "https://abc:def@example.org:81/a/b/c?query#fragment"_kj));
    KJ_ASSERT(url.getProtocol() == "https:"_kj);
    KJ_ASSERT(url.getUsername() == "abc"_kj);
    KJ_ASSERT(url.getPassword() == "def"_kj);
    KJ_ASSERT(url.getHost() == "example.org:81"_kj);
    KJ_ASSERT(url.getPathname() == "/xyz"_kj);
    KJ_ASSERT(url.getSearch() == kj::String());
    KJ_ASSERT(url.getHash() == kj::String());
  }

  {
    auto url = KJ_ASSERT_NONNULL(Url::tryParse("../../../../../././../../././../.././abc"_kj,
        "https://abc:def@example.org:81/a/b/c?query#fragment"_kj));
    KJ_ASSERT(url.getPathname() == "/abc"_kj);
  }

  {
    auto url = Url::tryParse("/anything"_kj, "data:cannot-be-base"_kj);
    KJ_ASSERT(url == kj::none);
  }
}

KJ_TEST("Can parse") {
  {
    KJ_ASSERT(Url::canParse("http://example.org"_kj));
    KJ_ASSERT(Url::canParse("foo"_kj, "http://example.org"_kj));
    KJ_ASSERT(!Url::canParse("this is not a parseable URL"_kj));
    KJ_ASSERT(!Url::canParse("foo"_kj, "base is not a URL"_kj));
  }
}

KJ_TEST("Normalize path for comparison and cloning") {
  // The URL parser does not percent-decode characters in the result.
  // For instance, even tho `f` does not need to be percent encoded,
  // the value `%66oo` will be returned as is. In some cases we want
  // to be able to treat `%66oo` and `foo` as equivalent for the sake
  // of comparison and cloning. This is what the NORMALIZE_PATH option
  // is for. It will percent-decode the path, then re-encode it.
  // Note that there is a definite performance cost to this, so it
  // should only be used when necessary.

  auto url1 = "file:///%66oo/boo%fe"_url;
  auto url2 = "file:///foo/boo%fe"_url;
  auto url3 = "file:///foo/boo%FE"_url;

  auto url4 = url1.clone(Url::EquivalenceOption::NORMALIZE_PATH);

  KJ_ASSERT(url1.equal(url2, Url::EquivalenceOption::NORMALIZE_PATH));
  KJ_ASSERT(url2.equal(url1, Url::EquivalenceOption::NORMALIZE_PATH));
  KJ_ASSERT(url3 == url4);

  // This one will not be equivalent because the %2f is not decoded
  auto url5 = KJ_ASSERT_NONNULL(Url::tryParse("file:///foo%2fboo%fe"_kj));

  KJ_ASSERT(!url5.equal(url2, Url::EquivalenceOption::NORMALIZE_PATH));

  auto url6 = url5.clone(Url::EquivalenceOption::NORMALIZE_PATH);
  KJ_ASSERT(url6.getHref() == "file:///foo%2Fboo%FE"_kj);

  auto url7 = "file:///foo%2Fboo%2F"_url;
  url7 = url7.clone(Url::EquivalenceOption::NORMALIZE_PATH);
  KJ_ASSERT(url7.getHref() == "file:///foo%2Fboo%2F"_kj);

  auto url8 = "file:///foo%2F%2f/bar"_url;
  url8 = url8.clone(Url::EquivalenceOption::NORMALIZE_PATH);
  KJ_ASSERT(url8.getHref() == "file:///foo%2F%2F/bar"_kj);

  auto url9 = "file:///foo%2f%2F/bar"_url;
  url9 = url9.clone(Url::EquivalenceOption::NORMALIZE_PATH);
  KJ_ASSERT(url9.getHref() == "file:///foo%2F%2F/bar"_kj);
}

}  // namespace
}  // namespace workerd::jsg::test
