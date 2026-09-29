// A Blob's bytes are a view of an ArrayBuffer, so they always lie inside the V8 sandbox, and
// Blob::getData() verifies that before any copy. This covers the genuine case, which must read
// normally. The check's failure path is observable only where V8 is built with the sandbox, so
// it is tested in edgeworker.

#include <workerd/api/blob.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

constexpr kj::StringPtr TYPE = "application/octet-stream"_kj;

KJ_TEST("Blob whose bytes lie inside the V8 sandbox reads normally") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto buffer = jsg::JsArrayBuffer::create(js, "hello"_kjb);
    auto blob = js.alloc<Blob>(js, jsg::JsBufferSource(buffer), kj::str(TYPE));
    KJ_EXPECT(blob->getData(js) == "hello"_kjb);

    auto sliced = blob->slice(js, 1, 4, kj::none);
    KJ_EXPECT(sliced->getData(js) == "ell"_kjb);
  });
}

}  // namespace
}  // namespace workerd::api
