// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"
#include "url.h"
#include <kj/table.h>

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

}  // namespace
}  // namespace workerd::jsg::test
