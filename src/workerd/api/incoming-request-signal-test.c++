// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/http.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

kj::Exception disconnected() {
  return JSG_KJ_EXCEPTION(DISCONNECTED, DOMAbortError, "The client has disconnected");
}

kj::String reasonMessage(jsg::Lock& js, AbortSignal& signal) {
  auto reason = KJ_ASSERT_NONNULL(signal.getReason(js).tryCast<jsg::JsObject>());
  return reason.get(js, "message"_kj).toString(js);
}

KJ_TEST("IncomingRequestSignal creates its signal once, on first use") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto incoming = js.alloc<IncomingRequestSignal>(AbortSignal::Flag::IGNORE_FOR_SUBREQUESTS);
    auto signal = incoming->get(js);
    KJ_EXPECT(incoming->get(js).get() == signal.get());
    KJ_EXPECT(!signal->getAborted(js));

    incoming->triggerAbort(js, disconnected());
    KJ_EXPECT(signal->getAborted(js));
    KJ_EXPECT(reasonMessage(js, *signal) == "The client has disconnected");
    KJ_EXPECT(incoming->get(js).get() == signal.get());
  });
}

KJ_TEST("IncomingRequestSignal created after an abort starts aborted") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto incoming = js.alloc<IncomingRequestSignal>(AbortSignal::Flag::IGNORE_FOR_SUBREQUESTS);
    incoming->triggerAbort(js, disconnected());

    auto signal = incoming->get(js);
    KJ_EXPECT(signal->getAborted(js));
    KJ_EXPECT(reasonMessage(js, *signal) == "The client has disconnected");
    KJ_EXPECT(incoming->get(js).get() == signal.get());
  });
}

}  // namespace
}  // namespace workerd::api
