// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/body-source.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

constexpr uint64_t kLimit = 1024 * 1024;

// "héllo 😀": 1 + 2 + 3 + 1 + 4 bytes of UTF-8 around the ASCII.
constexpr kj::StringPtr kText = "h\xc3\xa9llo \xf0\x9f\x98\x80"_kj;

TestFixture makeFixture(bool typescript) {
  if (!typescript) {
    return TestFixture();
  }
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setTypeScriptImplementedStreams(true);
  return TestFixture({.featureFlags = flags.asReader()});
}

// One source of each kind, all holding kText.
kj::Vector<BodySource> makeSources(jsg::Lock& js) {
  kj::Vector<BodySource> sources;
  sources.add(BodySource(kj::heapArray(kText.asBytes())));
  sources.add(BodySource(kj::str(kText)));
  sources.add(BodySource(js, js.str(kText)));
  sources.add(BodySource(js, jsg::JsBufferSource(jsg::JsUint8Array::create(js, kText.asBytes()))));
  sources.add(BodySource(js.alloc<Blob>(
      js, jsg::JsBufferSource(jsg::JsUint8Array::create(js, kText.asBytes())), kj::str())));
  return sources;
}

KJ_TEST("BodySource reports the UTF-8 length of every kind of source") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto& source: makeSources(js)) {
      KJ_EXPECT(source.tryGetLength(js, StreamEncoding::IDENTITY) == kText.size());
      KJ_EXPECT(source.tryGetLength(js, StreamEncoding::GZIP) == kj::none);
      KJ_EXPECT(source.clone(js).tryGetLength(js, StreamEncoding::IDENTITY) == kText.size());
    }
  });
}

KJ_TEST("BodySource counts a lone surrogate as the three bytes of U+FFFD") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    const char16_t units[] = {u'a', 0xd800, u'b'};
    BodySource source(js, js.str(kj::ArrayPtr<const char16_t>(units, kj::size(units))));
    KJ_EXPECT(source.tryGetLength(js, StreamEncoding::IDENTITY) == uint64_t{5});
  });
}

KJ_TEST("BodySource creates a new stream over the whole source each time") {
  for (bool typescript: {false, true}) {
    auto fixture = makeFixture(typescript);
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto& js = env.js;
      kj::Vector<jsg::Promise<void>> reads;
      for (auto& source: makeSources(js)) {
        auto first = source.createStream(js);
        auto second = source.clone(js).createStream(js);
        auto promise = first.text(js, kLimit)
                           .then(js,
                             JSG_VISITABLE_LAMBDA((source = kj::mv(source), typescript), (source),
                                 (jsg::Lock& js, kj::String text) {
                                   KJ_EXPECT(text == kText, typescript);
                                   // Reading one stream leaves the source whole for the next.
                                   return source.createStream(js).text(js, kLimit);
                                 }))
                           .then(js,
                             JSG_VISITABLE_LAMBDA((second = kj::mv(second), typescript), (second),
                                 (jsg::Lock& js, kj::String text) {
                                   KJ_EXPECT(text == kText, typescript);
                                   return second.text(js, kLimit);
                                 }))
                           .then(js, [typescript](jsg::Lock& js, kj::String text) {
          KJ_EXPECT(text == kText, typescript);
        });
        reads.add(kj::mv(promise));
      }
      kj::Vector<kj::Promise<void>> awaited;
      for (auto& read: reads) {
        awaited.add(env.context.awaitJs(js, kj::mv(read)));
      }
      return kj::joinPromises(awaited.releaseAsArray());
    });
  }
}

}  // namespace
}  // namespace workerd::api
