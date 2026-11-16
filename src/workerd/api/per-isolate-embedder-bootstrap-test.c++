// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/per-isolate-bootstrap.h>
#include <workerd/tests/test-fixture.h>

#include <embedder_bootstrap_test/embedder_bootstrap_test.capnp.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd {
namespace {

jsg::JsObject getExports(jsg::Lock& js, kj::StringPtr specifier) {
  auto exports = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, specifier), specifier);
  return KJ_ASSERT_NONNULL(exports.tryCast<jsg::JsObject>(), specifier);
}

KJ_TEST("embedder bootstrap scripts run after workerd's, in the same bootstrap") {
  EmbedderBootstrap embedder(EMBEDDER_BOOTSTRAP_TEST_BUNDLE, "test-embedder/");

  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setTypeScriptImplementedStreams(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .embedderBootstrap = embedder,
  });
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto exports = getExports(js, "test-embedder/main");

    // The embedder's require() resolves its own scripts, by their prefixed specifiers, and
    // workerd's, sharing workerd's cache.
    KJ_EXPECT(exports.get(js, "helper").strictEquals(getExports(js, "test-embedder/helper")));
    KJ_EXPECT(exports.get(js, "workerdMain").strictEquals(getExports(js, "main")));
    KJ_EXPECT(exports.get(js, "ringBufferLength").strictEquals(js.num(0)));
    KJ_EXPECT(tryGetBootstrapExport(js, "webstreams/ring-buffer") != kj::none);

    // The embedder's scripts get the same pseudo-globals as workerd's, and run before the WeakRef
    // globals are removed.
    KJ_EXPECT(exports.get(js, "hasUtils").strictEquals(js.boolean(true)));
    KJ_EXPECT(exports.get(js, "sawCompatFlag").strictEquals(js.boolean(true)));
    KJ_EXPECT(exports.get(js, "weakRefType").strictEquals(js.str("function"_kj)));
    KJ_EXPECT(js.global().get(js, "WeakRef").isUndefined());

    // What the embedder's scripts install on the global is visible to user code.
    KJ_EXPECT(js.global().get(js, "testEmbedderGlobal").strictEquals(js.str("installed"_kj)));

    // The embedder's require() stays usable after the bootstrap. A missing script in the
    // embedder's namespace is not looked up in workerd's.
    auto requireLater =
        KJ_ASSERT_NONNULL(exports.get(js, "requireLater").tryCast<jsg::JsFunction>());
    KJ_EXPECT(requireLater.call(js, js.undefined(), js.str("webstreams/ring-buffer"_kj))
                  .strictEquals(getExports(js, "webstreams/ring-buffer")));
    KJ_EXPECT_LOG(ERROR, "per-isolate bootstrap: unknown script");
    bool threw = false;
    js.tryCatch([&]() {
      std::ignore =
          requireLater.call(js, js.undefined(), js.str("test-embedder/webstreams/ring-buffer"_kj));
    }, [&](jsg::Value) { threw = true; });
    KJ_EXPECT(threw, "requiring a missing embedder script should throw");
  });
}

KJ_TEST("embedder bootstrap scripts don't run without an embedder") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    KJ_EXPECT(tryGetBootstrapExport(js, "main") != kj::none);
    KJ_EXPECT(tryGetBootstrapExport(js, "test-embedder/main") == kj::none);
    KJ_EXPECT(js.global().get(js, "testEmbedderGlobal").isUndefined());
  });
}

KJ_TEST("EmbedderBootstrap checks its bundle against its prefix") {
  KJ_EXPECT_THROW_MESSAGE("must be a non-empty path ending with '/'",
      EmbedderBootstrap(EMBEDDER_BOOTSTRAP_TEST_BUNDLE, "test-embedder"));
  KJ_EXPECT_THROW_MESSAGE("must be a non-empty path ending with '/'",
      EmbedderBootstrap(EMBEDDER_BOOTSTRAP_TEST_BUNDLE, "/"));
  KJ_EXPECT_THROW_MESSAGE("overlaps workerd's bootstrap scripts",
      EmbedderBootstrap(EMBEDDER_BOOTSTRAP_TEST_BUNDLE, "webstreams/"));
  KJ_EXPECT_THROW_MESSAGE(
      "not under its prefix", EmbedderBootstrap(EMBEDDER_BOOTSTRAP_TEST_BUNDLE, "other/"));
}

}  // namespace
}  // namespace workerd
