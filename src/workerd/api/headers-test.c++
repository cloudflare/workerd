// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/headers.h>
#include <workerd/jsg/memory.h>
#include <workerd/jsg/ser.h>
#include <workerd/tests/test-fixture.h>

#include <v8-profiler.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

// Headers made from a kj::HttpHeaders keep a snapshot of them until they're changed. These tests
// check that every read gives the same result from the snapshot as from the converted form.

// Names in various spellings: common and uncommon, some registered in the kj table and some not,
// and some that differ only in case.
constexpr kj::StringPtr NAMES[] = {
  "Content-Type"_kj,
  "content-type"_kj,
  "CONTENT-TYPE"_kj,
  "Accept"_kj,
  "accept-encoding"_kj,
  "Set-Cookie"_kj,
  "set-cookie"_kj,
  "Cookie"_kj,
  "Host"_kj,
  "Vary"_kj,
  "x-foo"_kj,
  "X-Foo"_kj,
  "X-FOO"_kj,
  "X-Bar"_kj,
  "x-bar"_kj,
  "zeta"_kj,
  "Alpha"_kj,
  "alpha"_kj,
  "X-Indexed"_kj,
  "x-indexed"_kj,
  "Cf-Ray"_kj,
  "a"_kj,
  "Z"_kj,
};

// Never in the generated headers, so deleting it converts a snapshot without changing anything.
constexpr auto ABSENT = capnp::CommonHeaderName::WWW_AUTHENTICATE;

struct Random {
  uint64_t state;
  uint next(uint bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state % bound;
  }
};

kj::String serializeKj(const kj::HttpHeaders& headers) {
  kj::Vector<kj::String> parts;
  headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
    parts.add(kj::str(name, ": ", value, "\n"));
  });
  return kj::strArray(parts, "");
}

kj::String displayed(jsg::Lock& js, Headers& headers) {
  return kj::strArray(KJ_MAP(header, headers.getDisplayedHeaders(js)) {
    return kj::str(header.key, ": ", header.value, "\n");
  }, "");
}

kj::Array<kj::byte> serializeJs(jsg::Lock& js, Headers& headers) {
  jsg::Serializer serializer(js);
  headers.serialize(js, serializer);
  return serializer.release().data;
}

void expectSameReads(jsg::Lock& js,
    const kj::HttpHeaderTable& table,
    Headers& snapshot,
    Headers& converted,
    kj::StringPtr context) {
  KJ_EXPECT(displayed(js, snapshot) == displayed(js, converted), context);

  kj::HttpHeaders a(table);
  kj::HttpHeaders b(table);
  snapshot.shallowCopyTo(a);
  converted.shallowCopyTo(b);
  KJ_EXPECT(serializeKj(a) == serializeKj(b), context, serializeKj(a), serializeKj(b));
  KJ_EXPECT(a.serializeResponse(200, "OK") == b.serializeResponse(200, "OK"), context);

  KJ_EXPECT(serializeJs(js, snapshot) == serializeJs(js, converted), context);

  for (auto name: NAMES) {
    KJ_EXPECT(snapshot.getPtr(js, name) == converted.getPtr(js, name), context, name);
    KJ_EXPECT(snapshot.has(kj::str(name)) == converted.has(kj::str(name)), context, name);
  }
  for (uint id = 1; id <= Headers::MAX_COMMON_HEADER_ID; id++) {
    auto common = static_cast<capnp::CommonHeaderName>(id);
    KJ_EXPECT(snapshot.getCommon(js, common) == converted.getCommon(js, common), context, id);
    KJ_EXPECT(snapshot.hasCommon(common) == converted.hasCommon(common), context, id);
  }
  KJ_EXPECT(
      kj::strArray(snapshot.getSetCookie(), "|") == kj::strArray(converted.getSetCookie(), "|"),
      context);
}

KJ_TEST("Headers from kj::HttpHeaders read the same before and after conversion") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    kj::HttpHeaderTable::Builder builder;
    builder.add("X-Indexed");
    builder.add("Set-Cookie");
    builder.add("Vary");
    auto table = builder.build();

    Random random{0x9e3779b97f4a7c15};
    for (uint round = 0; round < 2000; round++) {
      kj::HttpHeaders source(*table);
      // Mostly small sets, sometimes more entries than a snapshot holds.
      uint count = random.next(round % 10 == 0 ? 90 : 20);
      for (uint i = 0; i < count; i++) {
        auto name = NAMES[random.next(kj::size(NAMES))];
        auto value = kj::str("v", round, "-", i, random.next(3) == 0 ? "" : " x");
        source.addPtr(name, kj::mv(value));
      }
      auto context = kj::str("round ", round, ":\n", serializeKj(source));

      auto snapshot = js.alloc<Headers>(js, source, Headers::Guard::NONE);
      auto converted = js.alloc<Headers>(js, source, Headers::Guard::NONE);
      converted->deleteCommon(ABSENT);
      expectSameReads(js, *table, *snapshot, *converted, context);

      // A copy shares the snapshot. Changing the copy must not change the original.
      auto copy = js.alloc<Headers>(js, *snapshot);
      expectSameReads(js, *table, *copy, *converted, context);
      copy->set(js, kj::str("X-Foo"), kj::str("changed"));
      copy->append(js, kj::str("Set-Cookie"), kj::str("changed"));
      copy->delete_(kj::str("Content-Type"));
      expectSameReads(js, *table, *snapshot, *converted, context);

      // Changing either form the same way gives the same result.
      for (Headers* headers: {&*snapshot, &*converted}) {
        headers->append(js, kj::str("x-Bar"), kj::str("appended"));
        headers->set(js, kj::str("Accept"), kj::str("set"));
        headers->delete_(kj::str("zeta"));
      }
      expectSameReads(js, *table, *snapshot, *converted, context);
    }
  });
}

KJ_TEST("Headers snapshots outlive the kj::HttpHeaders they were made from") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    kj::HttpHeaderTable table;

    kj::Maybe<jsg::Ref<Headers>> headers;
    {
      auto source = kj::heap<kj::HttpHeaders>(table);
      source->addPtr("X-Foo", kj::str("foo"));
      source->addPtr("Set-Cookie", kj::str("a=1"));
      source->addPtr("Set-Cookie", kj::str("b=2"));
      headers = js.alloc<Headers>(js, *source, Headers::Guard::REQUEST);
      source->clear();
      source->addPtr("X-Foo", kj::str("overwritten"));
    }
    auto& h = KJ_ASSERT_NONNULL(headers);
    KJ_EXPECT(KJ_ASSERT_NONNULL(h->getPtr(js, "x-foo")) == "foo");
    KJ_EXPECT(kj::strArray(h->getSetCookie(), "|") == "a=1|b=2");

    // The copy for a startup snapshot, which may be made without the isolate lock, copies the
    // snapshot rather than sharing it. Under ASan, dropping it first catches a shared buffer.
    KJ_ASSERT_NONNULL(h->snapshotClone());

    // Guarded headers stay immutable, and a failed change leaves them as they were.
    KJ_EXPECT_THROW_MESSAGE("immutable", h->set(js, kj::str("X-Foo"), kj::str("bar")));
    KJ_EXPECT(KJ_ASSERT_NONNULL(h->getPtr(js, "x-foo")) == "foo");

    // Invalid names are rejected the same way either form.
    KJ_EXPECT_THROW_MESSAGE("Invalid header name", h->getPtr(js, "bad name"));
    KJ_EXPECT_THROW_MESSAGE("Invalid header name", h->has(kj::str("bad name")));
  });
}

KJ_TEST("Copies of Headers share one snapshot node in a heap snapshot") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    kj::HttpHeaderTable table;
    kj::HttpHeaders source(table);
    constexpr size_t VALUE_SIZE = 4096;
    auto big = kj::heapString(VALUE_SIZE);
    big.asArray().fill('v');
    source.addPtr("X-Big", kj::mv(big));

    auto original = js.alloc<Headers>(js, source, Headers::Guard::NONE);
    kj::Vector<jsg::Ref<Headers>> all;
    all.add(original.addRef());
    for (auto i KJ_UNUSED: kj::zeroTo(10)) all.add(js.alloc<Headers>(js, *original));
    auto& handler = KJ_REQUIRE_NONNULL(js.tryGetTypeHandler<jsg::Ref<Headers>>());
    for (auto& headers: all) {
      KJ_ASSERT(handler.wrap(js, headers.addRef())->IsObject());
    }

    static const jsg::HeapSnapshotDeleter deleter;
    kj::Own<const v8::HeapSnapshot> heap(
        js.v8Isolate->GetHeapProfiler()->TakeHeapSnapshot(nullptr, nullptr, true, true), deleter);

    kj::Vector<const v8::HeapGraphNode*> nodes;
    for (int i = 0; i < heap->GetNodesCount(); i++) {
      auto node = heap->GetNode(i);
      if (kj::str(node->GetName()) == "workerd / HeadersSnapshot") nodes.add(node);
    }
    KJ_ASSERT(nodes.size() == 1, nodes.size());
    auto shared = nodes[0];

    size_t retainers = 0;
    for (int i = 0; i < heap->GetNodesCount(); i++) {
      auto node = heap->GetNode(i);
      for (int c = 0; c < node->GetChildrenCount(); c++) {
        if (node->GetChild(c)->GetToNode() == shared) ++retainers;
      }
    }
    KJ_EXPECT(retainers == all.size(), retainers);

    size_t size = shared->GetShallowSize();
    for (int c = 0; c < shared->GetChildrenCount(); c++) {
      size += shared->GetChild(c)->GetToNode()->GetShallowSize();
    }
    KJ_EXPECT(size >= VALUE_SIZE, size);
  });
}

}  // namespace
}  // namespace workerd::api
