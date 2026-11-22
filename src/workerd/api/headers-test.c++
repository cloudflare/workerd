// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/headers.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

kj::String displayed(jsg::Lock& js, Headers& headers) {
  return kj::strArray(
      KJ_MAP(h, headers.getDisplayedHeaders(js)) { return kj::str(h.key, ": ", h.value); }, "\n");
}

// Request headers as kj parses them: some names the table indexes as common headers, one it
// indexes without it being a common header, and repeated names of both kinds.
constexpr kj::StringPtr REQUEST = "GET / HTTP/1.1\r\n"
                                  "Host: example.com\r\n"
                                  "Accept: text/html\r\n"
                                  "accept: */*\r\n"
                                  "CF-Ray: abc\r\n"
                                  "X-Custom: one\r\n"
                                  "x-custom: two\r\n"
                                  "Set-Cookie: a=1\r\n"
                                  "set-cookie: b=2\r\n"
                                  "X-Other: three\r\n"
                                  "\r\n"_kj;

kj::String requestDisplayed(jsg::Lock& js) {
  // Set-Cookie values are only listed separately with the getSetCookie() compat flag.
  auto setCookie = FeatureFlags::get(js).getHttpHeadersGetSetCookie()
      ? "set-cookie: a=1\nset-cookie: b=2\n"_kj
      : "set-cookie: a=1, b=2\n"_kj;
  return kj::str("accept: text/html, */*\n"
                 "cf-ray: abc\n"
                 "host: example.com\n",
      setCookie,
      "x-custom: one, two\n"
      "x-other: three");
}

void expectRequestHeaders(jsg::Lock& js, Headers& headers) {
  KJ_EXPECT(KJ_ASSERT_NONNULL(headers.get(js, kj::str("ACCEPT"))) == "text/html, */*");
  KJ_EXPECT(KJ_ASSERT_NONNULL(headers.get(js, kj::str("cf-ray"))) == "abc");
  KJ_EXPECT(KJ_ASSERT_NONNULL(headers.get(js, kj::str("X-CUSTOM"))) == "one, two");
  KJ_EXPECT(KJ_ASSERT_NONNULL(headers.get(js, kj::str("set-cookie"))) == "a=1, b=2");
  KJ_EXPECT(
      KJ_ASSERT_NONNULL(headers.getCommon(js, capnp::CommonHeaderName::HOST)) == "example.com");
  KJ_EXPECT(headers.has(kj::str("x-other")));
  KJ_EXPECT(!headers.has(kj::str("x-missing")));
  KJ_EXPECT(!headers.hasCommon(capnp::CommonHeaderName::AUTHORIZATION));
  auto cookies = headers.getSetCookie();
  KJ_ASSERT(cookies.size() == 2);
  KJ_EXPECT(cookies[0] == "a=1");
  KJ_EXPECT(cookies[1] == "b=2");
  // None of the reads above needed storage of the object's own.
  KJ_EXPECT(headers.isNativeForTest());

  // Copies share the native copy until they're changed.
  auto copy = js.alloc<Headers>(js, headers);
  auto clone = headers.clone(js);
  KJ_EXPECT(copy->isNativeForTest());
  KJ_EXPECT(clone->isNativeForTest());
  copy->append(js, kj::str("X-Custom"), kj::str("three"));
  KJ_EXPECT(!copy->isNativeForTest());
  KJ_EXPECT(KJ_ASSERT_NONNULL(copy->get(js, kj::str("x-custom"))) == "one, two, three");
  KJ_EXPECT(KJ_ASSERT_NONNULL(headers.get(js, kj::str("x-custom"))) == "one, two");
  KJ_EXPECT(KJ_ASSERT_NONNULL(clone->get(js, kj::str("x-custom"))) == "one, two");
  KJ_EXPECT(headers.isNativeForTest());

  // Iterating materializes, and produces what reading the native copy does.
  KJ_EXPECT(displayed(js, headers) == requestDisplayed(js), displayed(js, headers));
  KJ_EXPECT(!headers.isNativeForTest());
  KJ_EXPECT(displayed(js, *clone) == requestDisplayed(js), displayed(js, *clone));
  KJ_EXPECT(KJ_ASSERT_NONNULL(headers.get(js, kj::str("x-custom"))) == "one, two");
  KJ_EXPECT(headers.getSetCookie().size() == 2);
}

KJ_TEST("Headers built from kj::HttpHeaders on the thread's table") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    kj::HttpHeaders kjHeaders(env.context.getHeaderTable());
    auto in = kj::heapString(REQUEST);
    KJ_ASSERT(kjHeaders.tryParseRequest(in.asArray()).is<kj::HttpHeaders::Request>());

    auto headers = js.alloc<Headers>(js, kjHeaders, Headers::Guard::REQUEST);
    expectRequestHeaders(js, *headers);
  });
}

KJ_TEST("Headers built from kj::HttpHeaders on a different table") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    // A table that assigns ids in a different order than the thread's table, so that ids which
    // name common headers in one table name other headers in the other.
    kj::HttpHeaderTable::Builder builder;
    builder.add("X-Custom");
    builder.add("Set-Cookie");
    builder.add("CF-Ray");
    builder.add("Accept");
    auto table = builder.build();
    kj::HttpHeaders kjHeaders(*table);
    auto in = kj::heapString(REQUEST);
    KJ_ASSERT(kjHeaders.tryParseRequest(in.asArray()).is<kj::HttpHeaders::Request>());

    auto headers = js.alloc<Headers>(js, kjHeaders, Headers::Guard::REQUEST);
    expectRequestHeaders(js, *headers);
  });
}

KJ_TEST("Headers::shallowCopyTo() writes what adding each value by name would") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto& table = env.context.getHeaderTable();
    auto& ids = env.context.getHeaderIds().commonHeaderIds;

    auto headers = js.alloc<Headers>();
    headers->append(js, kj::str("Accept"), kj::str("a"));
    headers->append(js, kj::str("accept"), kj::str("b"));
    headers->append(js, kj::str("Cache-Control"), kj::str(""));
    headers->append(js, kj::str("Cache-Control"), kj::str("no-cache"));
    headers->append(js, kj::str("Set-Cookie"), kj::str("c1"));
    headers->append(js, kj::str("set-cookie"), kj::str("c2"));
    headers->append(js, kj::str("Set-Cookie"), kj::str("c3"));
    headers->append(js, kj::str("Vary"), kj::str("x"));
    headers->append(js, kj::str("Vary"), kj::str(""));
    headers->append(js, kj::str("Age"), kj::str(""));
    headers->append(js, kj::str("X-B"), kj::str("1"));
    headers->append(js, kj::str("x-a"), kj::str("2"));
    headers->append(js, kj::str("X-A"), kj::str("3"));

    // What Headers produced when it added every value by name: common headers in
    // CommonHeaderName order, then the rest sorted by name.
    auto addAll = [](kj::HttpHeaders& out) {
      out.addPtrPtr("Accept", "a");
      out.addPtrPtr("Accept", "b");
      out.addPtrPtr("age", "");
      out.addPtrPtr("Cache-Control", "");
      out.addPtrPtr("Cache-Control", "no-cache");
      out.addPtrPtr("Set-Cookie", "c1");
      out.addPtrPtr("Set-Cookie", "c2");
      out.addPtrPtr("Set-Cookie", "c3");
      out.addPtrPtr("Vary", "x");
      out.addPtrPtr("Vary", "");
      out.addPtrPtr("X-B", "1");
      out.addPtrPtr("x-a", "2");
      out.addPtrPtr("x-a", "3");
    };

    {
      kj::HttpHeaders expected(table);
      addAll(expected);
      kj::HttpHeaders out(table);
      headers->shallowCopyTo(out, ids);
      KJ_EXPECT(out.serializeResponse(200, "OK") == expected.serializeResponse(200, "OK"),
          out.toString(), expected.toString());
    }

    {
      // A header that `out` already holds is merged with, as addPtrPtr() would.
      kj::HttpHeaders expected(table);
      expected.setPtr(ids.toKj(capnp::CommonHeaderName::ACCEPT), "z");
      addAll(expected);
      kj::HttpHeaders out(table);
      out.setPtr(ids.toKj(capnp::CommonHeaderName::ACCEPT), "z");
      headers->shallowCopyTo(out, ids);
      KJ_EXPECT(out.serializeResponse(200, "OK") == expected.serializeResponse(200, "OK"),
          out.toString(), expected.toString());
    }
  });
}

KJ_TEST("Headers built from kj::HttpHeaders materialize on mutation, guarded or not") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    kj::HttpHeaders kjHeaders(env.context.getHeaderTable());
    auto in = kj::heapString(REQUEST);
    KJ_ASSERT(kjHeaders.tryParseRequest(in.asArray()).is<kj::HttpHeaders::Request>());

    auto guarded = js.alloc<Headers>(js, kjHeaders, Headers::Guard::REQUEST);
    KJ_EXPECT_THROW_MESSAGE(
        "Can't modify immutable headers.", guarded->set(js, kj::str("x-custom"), kj::str("v")));
    KJ_EXPECT_THROW_MESSAGE("Can't modify immutable headers.", guarded->delete_(kj::str("accept")));
    KJ_EXPECT(guarded->isNativeForTest());

    // Internal mutation bypasses the guard.
    guarded->setCommon(capnp::CommonHeaderName::CONTENT_LENGTH, kj::str("5"));
    KJ_EXPECT(!guarded->isNativeForTest());
    KJ_EXPECT(KJ_ASSERT_NONNULL(guarded->get(js, kj::str("content-length"))) == "5");
    KJ_EXPECT(KJ_ASSERT_NONNULL(guarded->get(js, kj::str("accept"))) == "text/html, */*");

    auto mutable_ = js.alloc<Headers>(js, kjHeaders, Headers::Guard::NONE);
    mutable_->delete_(kj::str("Set-Cookie"));
    KJ_EXPECT(!mutable_->isNativeForTest());
    KJ_EXPECT(!mutable_->has(kj::str("set-cookie")));
    KJ_EXPECT(KJ_ASSERT_NONNULL(mutable_->get(js, kj::str("x-custom"))) == "one, two");
  });
}

KJ_TEST("Headers built from kj::HttpHeaders copy out what their materialized form would") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto& table = env.context.getHeaderTable();
    auto& ids = env.context.getHeaderIds().commonHeaderIds;

    // Headers with names that only differ in case, repeated common headers, and (from the table
    // that doesn't index Accept) a repeated common header that kj keeps unindexed.
    auto input = "GET / HTTP/1.1\r\n"
                 "x-b: 1\r\n"
                 "Accept: text/html\r\n"
                 "X-A: 2\r\n"
                 "Set-Cookie: a=1\r\n"
                 "x-a: 3\r\n"
                 "X-b: 4\r\n"
                 "accept: */*\r\n"
                 "SET-COOKIE: b=2\r\n"
                 "Cache-Control: no-cache\r\n"
                 "z: 5\r\n"
                 "\r\n"_kj;

    kj::HttpHeaderTable::Builder builder;
    builder.add("X-Custom");
    auto otherTable = builder.build();

    const kj::HttpHeaderTable* sourceTables[] = {&table, otherTable.get()};
    for (auto sourceTable: sourceTables) {
      kj::HttpHeaders kjHeaders(*sourceTable);
      auto in = kj::heapString(input);
      KJ_ASSERT(kjHeaders.tryParseRequest(in.asArray()).is<kj::HttpHeaders::Request>());

      auto native = js.alloc<Headers>(js, kjHeaders, Headers::Guard::REQUEST);
      auto materialized = js.alloc<Headers>(js, kjHeaders, Headers::Guard::REQUEST);
      materialized->getDisplayedHeaders(js);
      KJ_ASSERT(native->isNativeForTest());
      KJ_ASSERT(!materialized->isNativeForTest());

      kj::HttpHeaders fromNative(table);
      native->shallowCopyTo(fromNative, ids);
      kj::HttpHeaders fromMaterialized(table);
      materialized->shallowCopyTo(fromMaterialized, ids);
      KJ_EXPECT(native->isNativeForTest());
      KJ_EXPECT(
          fromNative.serializeResponse(200, "OK") == fromMaterialized.serializeResponse(200, "OK"),
          fromNative.toString(), fromMaterialized.toString());
      KJ_EXPECT(fromNative.toString().contains("X-A: 2\r\nX-A: 3\r\n"), fromNative.toString());
    }
  });
}

KJ_TEST("CommonHeaderIds maps between common header names and table ids") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& table = env.context.getHeaderTable();
    auto& ids = env.context.getHeaderIds().commonHeaderIds;
    for (uint i = 1; i <= CommonHeaderIds::MAX_COMMON_HEADER_ID; i++) {
      auto common = static_cast<capnp::CommonHeaderName>(i);
      auto id = ids.toKj(common);
      KJ_EXPECT((KJ_ASSERT_NONNULL(table.stringToId(id.toString())) == id));
      KJ_EXPECT((KJ_ASSERT_NONNULL(ids.fromKj(id, id.toString())) == common));
    }
    KJ_EXPECT((ids.fromKj(kj::HttpHeaderId::CONNECTION, kj::HttpHeaderId::CONNECTION.toString()) ==
        kj::none));

    // An id from another table is only trusted when that table holds the same name string.
    kj::HttpHeaderTable::Builder builder;
    auto custom = builder.add("X-Custom");
    auto other = builder.build();
    KJ_EXPECT((ids.fromKj(custom, custom.toString()) == kj::none));
    auto contentType = KJ_ASSERT_NONNULL(other->stringToId("content-type"));
    KJ_EXPECT((KJ_ASSERT_NONNULL(ids.fromKj(contentType, contentType.toString())) ==
        capnp::CommonHeaderName::CONTENT_TYPE));
  });
}

}  // namespace
}  // namespace workerd::api
