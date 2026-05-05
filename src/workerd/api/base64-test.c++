#include "base64.h"
#include "workerd/tests/test-fixture.h"

#include <kj/encoding.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

KJ_TEST("base64 encode") {
  TestFixture t;

  t.runInIoContext([](const workerd::TestFixture::Environment& env) {
    auto b = Base64Module();
    auto ab = jsg::JsArrayBuffer::create(env.js, 1);
    ab.asArrayPtr().begin()[0] = 'A';
    auto ret = b.encodeArray(env.js, jsg::JsBufferSource(ab));
    KJ_ASSERT(ret.asArrayPtr() == "QQ=="_kjb);
  });
}

KJ_TEST("base64 valid decode") {
  TestFixture t;

  t.runInIoContext([](const workerd::TestFixture::Environment& env) {
    auto b = Base64Module();
    auto ab = jsg::JsArrayBuffer::create(env.js, 4);
    ab.asArrayPtr().copyFrom("QQ=="_kjb);
    auto ret = b.decodeArray(env.js, jsg::JsBufferSource(ab));
    KJ_ASSERT(ret.asArrayPtr() == "A"_kjb);
  });
}

KJ_TEST("base64 invalid decode") {
  TestFixture t;

  t.runInIoContext([](const workerd::TestFixture::Environment& env) {
    auto b = Base64Module();
    auto ab = jsg::JsArrayBuffer::create(env.js, 14);
    ab.asArrayPtr().copyFrom("INVALID BASE64"_kjb);
    try {
      b.decodeArray(env.js, jsg::JsBufferSource(ab));
      KJ_UNREACHABLE;
    } catch (kj::Exception& e) {
      KJ_EXPECT(e.getDescription().contains("jsg.DOMException(SyntaxError): Invalid base64"_kj));
    }
  });
}

KJ_TEST("base64 decode as string") {
  TestFixture t;

  t.runInIoContext([](const workerd::TestFixture::Environment& env) {
    auto b = Base64Module();
    auto ab = jsg::JsArrayBuffer::create(env.js, 1);
    ab.asArrayPtr().begin()[0] = 'A';
    KJ_ASSERT(b.encodeArrayToString(env.js, jsg::JsBufferSource(ab)) == env.js.str("QQ=="_kj));
  });
}
}  // namespace
}  // namespace workerd::api
