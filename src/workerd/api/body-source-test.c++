// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/body-source.h>
#include <workerd/api/form-data.h>
#include <workerd/api/http.h>
#include <workerd/api/url-standard.h>
#include <workerd/api/url.h>
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
  sources.add(BodySource(js, kText.asBytes()));
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

KJ_TEST("A FormData or URLSearchParams body is held in memory the isolate accounts for") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    constexpr size_t kValueSize = 64 * 1024;
    auto value = kj::str(kj::repeat('a', kValueSize));
    auto formData = FormData::constructor(js);
    formData->append(js, kj::str("a"), kj::str(value), kj::none);
    auto legacyParams =
        URLSearchParams::constructor(js, URLSearchParams::Initializer(kj::str("a=", value)));
    auto params = url::URLSearchParams::constructor(
        js, url::URLSearchParams::Initializer(jsg::USVString(kj::str("a=", value))));
    // Each is passed by a new reference, so that the value, which the legacy URLSearchParams
    // accounts for, lives on through the measurement.
    auto initializers =
        kj::arr<Body::Initializer>(formData.addRef(), legacyParams.addRef(), params.addRef());
    for (auto i: kj::indices(initializers)) {
      // External memory counts an ArrayBuffer's backing store, and does not count native memory.
      int64_t before = js.v8Isolate->GetExternalMemory();
      auto body = Body::extractBody(js, kj::mv(initializers[i]));
      int64_t held = js.v8Isolate->GetExternalMemory() - before;
      KJ_EXPECT(held >= kValueSize, held, i);
    }
  });
}

}  // namespace
}  // namespace workerd::api
