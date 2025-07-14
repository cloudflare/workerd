#include "base64.h"
#include "workerd/tests/test-fixture.h"

#include <kj/encoding.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

KJ_TEST("base64 encode") {
  auto b = Base64Module();

  KJ_ASSERT(b.encodeArray(kj::heapArray("A"_kjb)) == "QQ=="_kjb);
}

KJ_TEST("base64 valid decode") {
  auto b = Base64Module();

  KJ_ASSERT(b.decodeArray(kj::heapArray("QQ=="_kjb)) == "A"_kjb);
}

KJ_TEST("base64 invalid decode") {
  auto b = Base64Module();

  try {
    b.decodeArray(kj::heapArray("INVALID BASE64"_kjb));
    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(e.getDescription().contains("jsg.DOMException(SyntaxError): Invalid base64"_kj));
  }
}

KJ_TEST("base64 decode as string") {
  auto t = TestFixture();

  t.runInIoContext([](const workerd::TestFixture::Environment& env) -> kj::Promise<void> {
    auto b = Base64Module();
    KJ_ASSERT(b.encodeArrayToString(env.lock, kj::heapArray("A"_kjb)) == env.js.str("QQ=="_kj));
    return kj::READY_NOW;
  });
}
}  // namespace
}  // namespace workerd::api
