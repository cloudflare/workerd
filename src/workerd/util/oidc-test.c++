// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "oidc.h"

#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("OIDC audiences accept exact StringOrURI values without URL normalization") {
  for (auto audience: {"recipient"_kj, "two words"_kj, " 雪 🔐 "_kj, "a\nb"_kj,
         "urn:example:recipient"_kj, "mailto:User@Example.COM"_kj, "custom:"_kj, "custom:/path"_kj,
         "https://EXAMPLE.com:443/a/../b?x=%2f#part"_kj, "file:///tmp/key"_kj,
         "custom://user:password@host:123/path"_kj, "custom://host:"_kj,
         "custom://reg!$&'()*+,;=name/"_kj, "https://[::1]/"_kj, "https://[2001:db8::1]:443/"_kj,
         "custom://[1:2:3:4:5:6:7:8]/"_kj, "custom://[::]/"_kj, "custom://[::ffff:192.0.2.1]/"_kj,
         "custom://[1:2:3:4:5:6:192.0.2.1]/"_kj, "custom://[v1.future:address]/"_kj,
         "urn:example:%E9%9B%AA"_kj}) {
    KJ_EXPECT(isValidOidcAudience(audience), audience);
  }
  auto limit = kj::heapString(2048);
  limit.asArray().fill('a');
  KJ_EXPECT(isValidOidcAudience(limit));
}

KJ_TEST("OIDC audiences reject malformed UTF-8, whitespace-only values and malformed URIs") {
  for (auto audience: {""_kj, " \t\r\n"_kj, "\xc2\xa0\xe3\x80\x80\xef\xbb\xbf"_kj, "\xc0\xaf"_kj,
         "\xed\xa0\x80"_kj, "\xf4\x90\x80\x80"_kj, ":missing-scheme"_kj, "1scheme:recipient"_kj,
         "bad scheme:recipient"_kj, "https://host/a b"_kj, "https://host/\"quote"_kj,
         "urn:example:雪"_kj, "urn:example:%"_kj, "urn:example:%0"_kj, "urn:example:%zz"_kj,
         "https://host/?a=%0g"_kj, "https://host/#a#b"_kj, "https://host/[]"_kj,
         "https://user@@host/"_kj, "https://host:abc/"_kj, "https://host:80:90/"_kj,
         "https://[::1"_kj, "https://[garbage]/"_kj, "https://[1:2:3]/"_kj,
         "https://[1:2:3:4:5:6:7:8:9]/"_kj, "https://[1:2:3:4:5:6:7:8::]/"_kj,
         "https://[1::2::3]/"_kj, "https://[1:2:3:4:5:6:7:]/"_kj, "https://[::ffff:256.0.0.1]/"_kj,
         "https://[::ffff:192.00.2.1]/"_kj, "https://[::ffff:192.0.2]/"_kj, "https://[v1.]/"_kj,
         "https://[vg.host]/"_kj, "https://[::1]suffix/"_kj, "https://host/\\path"_kj}) {
    KJ_EXPECT(!isValidOidcAudience(audience), audience);
  }
  auto oversized = kj::heapString(2049);
  oversized.asArray().fill('a');
  KJ_EXPECT(!isValidOidcAudience(oversized));
}

}  // namespace
}  // namespace workerd
