// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/global-scope.h>
#include <workerd/api/per-isolate-bootstrap.h>
#include <workerd/io/worker.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd {
namespace {

// Calls runtime-class's removeToStringTagUnlessFlagged() on `proto`, returning the message of the
// error it throws, if any. The function calls utils.getCompatFlags() each time it runs. Loading
// the module, if nothing has yet, reads no flags.
kj::Maybe<kj::String> tryRemoveToStringTag(jsg::Lock& js, jsg::JsObject proto) {
  auto exports =
      KJ_ASSERT_NONNULL(requireBootstrapExport(js, "runtime-class").tryCast<jsg::JsObject>());
  auto fn = KJ_ASSERT_NONNULL(
      exports.get(js, "removeToStringTagUnlessFlagged").tryCast<jsg::JsFunction>());
  kj::Maybe<kj::String> message;
  js.tryCatch([&]() { std::ignore = fn.call(js, js.undefined(), proto); },
      [&](jsg::Value exception) { message = jsg::JsValue(exception.getHandle(js)).toString(js); });
  return message;
}

KJ_TEST("utils.getCompatFlags() throws until the post-snapshot phase starts") {
  TestFixture fixture;
  fixture.enterWorkerLockSynchronously([](Worker::Lock& lock) {
    auto& js = jsg::Lock::from(lock.getIsolate());
    auto& api = lock.getWorker().getIsolate().getApi();
    js.withinHandleScope([&] {
      // A context of the test's own, on which it runs the two phases one at a time.
      auto context = api.newContext(js);
      auto v8Context = context.getHandle(js);
      KJ_DEFER(cleanupPerIsolateBootstrap(js, v8Context));
      JSG_WITHIN_CONTEXT_SCOPE(js, v8Context, [&](jsg::Lock& js) {
        runPerIsolateBootstrap(js);
        auto proto = js.obj();
        proto.set(js, js.symbolToStringTag(), js.str("Tagged"_kj));

        auto message = KJ_ASSERT_NONNULL(tryRemoveToStringTag(js, proto),
            "utils.getCompatFlags() should throw in the main phase");
        KJ_EXPECT(message.contains("cannot be called in the main phase"), message);
        KJ_EXPECT(proto.has(js, js.symbolToStringTag()));

        // The fixture leaves set_tostring_tag off, so once the flags can be read, the tag goes.
        runPostSnapshotBootstrap(js, api.getFeatureFlags());
        KJ_EXPECT(tryRemoveToStringTag(js, proto) == kj::none);
        KJ_EXPECT(!proto.has(js, js.symbolToStringTag()));
      });
    });
  });
}

}  // namespace
}  // namespace workerd
