// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "http-headers-patch.h"

#include <kj/test.h>

namespace workerd {
namespace {

struct Table {
  kj::HttpHeaderTable::Builder builder;
  kj::HttpHeaderId custom = builder.add("X-Custom");
  kj::Own<kj::HttpHeaderTable> table = builder.build();
};

KJ_TEST("HttpHeadersPatch forwards the original while no edit changes it") {
  Table t;
  kj::HttpHeaders headers(*t.table);
  headers.setPtr(kj::HttpHeaderId::CONTENT_TYPE, "text/plain");

  HttpHeadersPatch patch(headers);
  patch.unset(t.custom);
  patch.unset(kj::HttpHeaderId::HOST);
  patch.setPtr(kj::HttpHeaderId::CONTENT_TYPE, "text/plain"_kj);
  patch.set(kj::HttpHeaderId::CONTENT_TYPE, kj::str("text/plain"));
  patch.setPtr(kj::HttpHeaderId::LOCATION, ""_kj);
  patch.set(kj::HttpHeaderId::LOCATION, kj::String());
  KJ_EXPECT(!patch.isCopy());
  KJ_EXPECT(&patch.get() == &headers);
}

KJ_TEST("HttpHeadersPatch copies on the first change and leaves the original alone") {
  Table t;
  kj::HttpHeaders headers(*t.table);
  headers.setPtr(kj::HttpHeaderId::CONTENT_TYPE, "text/plain");
  headers.setPtr(t.custom, "secret");
  headers.addPtrPtr("X-Unindexed", "kept");
  auto before = headers.toString();

  HttpHeadersPatch patch(headers);
  patch.unset(t.custom);
  KJ_EXPECT(patch.isCopy());
  patch.setPtr(kj::HttpHeaderId::HOST, "example.com");
  patch.set(kj::HttpHeaderId::LOCATION, kj::str("/elsewhere"));
  // Setting an empty value removes, as for kj::HttpHeaders.
  patch.setPtr(kj::HttpHeaderId::CONTENT_TYPE, ""_kj);

  KJ_EXPECT(headers.toString() == before);
  KJ_EXPECT(patch.get().get(t.custom) == kj::none);
  KJ_EXPECT(patch.get().get(kj::HttpHeaderId::CONTENT_TYPE) == kj::none);
  KJ_EXPECT(KJ_ASSERT_NONNULL(patch.get().get(kj::HttpHeaderId::HOST)) == "example.com");
  KJ_EXPECT(KJ_ASSERT_NONNULL(patch.get().get(kj::HttpHeaderId::LOCATION)) == "/elsewhere");

  // The result is what the same edits make of a shallow copy.
  auto expected = headers.cloneShallow();
  expected.unset(t.custom);
  expected.setPtr(kj::HttpHeaderId::HOST, "example.com");
  expected.setPtr(kj::HttpHeaderId::LOCATION, "/elsewhere");
  expected.setPtr(kj::HttpHeaderId::CONTENT_TYPE, "");
  KJ_EXPECT(patch.get().toString() == expected.toString());
}

}  // namespace
}  // namespace workerd
