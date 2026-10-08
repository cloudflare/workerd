// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/blob.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

constexpr kj::StringPtr TYPE = "application/octet-stream"_kj;

KJ_TEST("Blob byte views preserve nested slices, empty views, and File payloads") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto buffer = jsg::JsArrayBuffer::create(js, "hello"_kjb);
    auto blob = js.alloc<Blob>(js, jsg::JsBufferSource(buffer), kj::str(TYPE));
    KJ_EXPECT(blob->getData(js) == "hello"_kjb);

    auto sliced = blob->slice(js, 1, 4, kj::none);
    KJ_EXPECT(sliced->getData(js) == "ell"_kjb);
    auto nested = sliced->slice(js, 1, 2, kj::none);
    KJ_EXPECT(nested->getData(js) == "l"_kjb);
    KJ_EXPECT(nested->getSize() == 1);

    auto empty = nested->slice(js, 0, 0, kj::none);
    KJ_EXPECT(empty->getData(js) == nullptr);
    KJ_EXPECT(empty->getSize() == 0);

    auto file =
        js.alloc<File>(js, blob.addRef(), blob->getData(js), kj::str("file"), kj::str(TYPE), 0);
    KJ_EXPECT(file->getData(js) == "hello"_kjb);

    auto zeroBuffer = jsg::JsArrayBuffer::create(js, 0);
    auto zeroBlob = js.alloc<Blob>(js, jsg::JsBufferSource(zeroBuffer), kj::str(TYPE));
    KJ_EXPECT(zeroBlob->getData(js) == nullptr);
  });
}

}  // namespace
}  // namespace workerd::api
