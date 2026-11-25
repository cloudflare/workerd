// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/per-isolate-bootstrap.h>
#include <workerd/jsg/setup.h>
#include <workerd/server/workerd-api.h>
#include <workerd/tests/test-fixture.h>

#include <embedder_bootstrap_test/embedder_bootstrap_test.capnp.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("baseline snapshot boots shared isolates with canonical and delta compatibility flags") {
  EmbedderBootstrap embedder(EMBEDDER_BOOTSTRAP_TEST_BUNDLE, "test-embedder/");
  auto baseline = server::WorkerdApi::buildBaselineSnapshot(
      TestFixture::getV8System(), kj::atomicRefcounted<jsg::SnapshotArtifact>(), embedder);
  KJ_EXPECT(baseline->blob.data != nullptr && baseline->blob.raw_size > 0);
  KJ_EXPECT(baseline->bootstrapStateIndex != kj::none);
  KJ_EXPECT(baseline->contextSnapshotIndex != kj::none);
  KJ_EXPECT(!baseline->constructorTemplateIndices.empty());

  // Isolate 1: canonical modern flags (no deletions).
  {
    capnp::MallocMessageBuilder message;
    auto flags = message.initRoot<CompatibilityFlags>();
    initCanonicalModernBaselineFlags(flags);
    KJ_EXPECT(canUseBaselineSnapshot(flags.asReader()));

    TestFixture fixture({
      .featureFlags = flags.asReader(),
      .embedderBootstrap = embedder,
      .snapshotConfig = jsg::SnapshotConfig(jsg::BaselineSnapshot{.artifact = baseline->addRef()}),
    });
    fixture.runInIoContext([](const TestFixture::Environment& env) {
      auto& js = env.js;
      KJ_EXPECT(js.isStartingFromBaselineSnapshot());
      KJ_EXPECT(!js.isStartingFromSnapshot());
      KJ_EXPECT(!js.global().get(js, "WeakRef").isUndefined());
      KJ_EXPECT(!js.global().get(js, "MessageChannel").isUndefined());
      KJ_EXPECT(!js.global().get(js, "Buffer").isUndefined());
      KJ_EXPECT(!js.global().get(js, "Performance").isUndefined());
      KJ_EXPECT(!js.global().get(js, "EventSource").isUndefined());
      KJ_EXPECT(js.global().get(js, "testEmbedderGlobal").strictEquals(js.str("installed"_kj)));
      KJ_EXPECT(tryGetBootstrapExport(js, "webstreams/streams") == kj::none);
    });
  }

  // Isolate 2: same baseline snapshot, with optional flags disabled via C++ delta deletion.
  {
    capnp::MallocMessageBuilder message;
    auto flags = message.initRoot<CompatibilityFlags>();
    initCanonicalModernBaselineFlags(flags);
    flags.setJsWeakRef(false);
    flags.setExposeGlobalMessageChannel(false);
    flags.setNodeJsCompatV2(false);
    flags.setGlobalNavigator(false);
    flags.setEnableGlobalPerformanceClasses(false);
    flags.setEnableNodeJsPerfHooksModule(false);
    flags.setHttpHeadersGetSetCookie(false);
    flags.setCacheOptionEnabled(false);
    flags.setSpecCompliantDispatchExceptions(false);
    KJ_EXPECT(canUseBaselineSnapshot(flags.asReader()));

    TestFixture fixture({
      .featureFlags = flags.asReader(),
      .embedderBootstrap = embedder,
      .snapshotConfig = jsg::SnapshotConfig(jsg::BaselineSnapshot{.artifact = baseline->addRef()}),
    });
    fixture.runInIoContext([](const TestFixture::Environment& env) {
      auto& js = env.js;
      KJ_EXPECT(js.isStartingFromBaselineSnapshot());
      KJ_EXPECT(!js.isStartingFromSnapshot());
      KJ_EXPECT(js.global().get(js, "WeakRef").isUndefined());
      KJ_EXPECT(js.global().get(js, "MessageChannel").isUndefined());
      KJ_EXPECT(js.global().get(js, "Buffer").isUndefined());
      KJ_EXPECT(js.global().get(js, "navigator").isUndefined());
      KJ_EXPECT(js.global().get(js, "Performance").isUndefined());
      KJ_EXPECT(!js.global().get(js, "ReadableStream").isUndefined());
      KJ_EXPECT(!js.global().get(js, "EventSource").isUndefined());
      KJ_EXPECT(js.global().get(js, "testEmbedderGlobal").strictEquals(js.str("installed"_kj)));
    });
  }
}

KJ_TEST("ephemeral zygote chains from baseline snapshot to produce a worker snapshot") {
  auto baseline = server::WorkerdApi::buildBaselineSnapshot(
      TestFixture::getV8System(), kj::atomicRefcounted<jsg::SnapshotArtifact>());

  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  initCanonicalModernBaselineFlags(flags);
  flags.setNodeJsCompatV2(false);
  flags.setExposeGlobalMessageChannel(false);
  KJ_EXPECT(canUseBaselineSnapshot(flags.asReader()));

  constexpr kj::StringPtr workerSource = R"(
    const token = Math.random();
    let counter = 0;
    export default {
      async fetch() {
        counter++;
        const sp = new URLSearchParams("a=héllo&b=world");
        const encoded = sp.toString();
        const es = EventSource.from(new ReadableStream({
          start(c) {
            c.enqueue("data: hi\n\n");
            c.close();
          }
        }));
        const msg = await new Promise((resolve) => {
          es.onmessage = (e) => { es.close(); resolve(e.data); };
        });
        const hasBuf = "Buffer" in globalThis;
        const hasMc = "MessageChannel" in globalThis;
        return new Response(`${token}:${counter}:${encoded}:${msg}:${hasBuf}:${hasMc}`);
      }
    };
  )"_kj;

  // Step 1: Build a per-worker snapshot in a zygote chained off the baseline snapshot.
  kj::Own<jsg::SnapshotArtifact> workerArtifact;
  {
    TestFixture zygote({
      .featureFlags = flags.asReader(),
      .mainModuleSource = workerSource,
      .snapshotConfig = jsg::SnapshotConfig(jsg::MutableSnapshot{
        .artifact = kj::atomicRefcounted<jsg::SnapshotArtifact>(),
        .base = jsg::BaselineSnapshot{.artifact = baseline->addRef()},
      }),
    });
    zygote.enterWorkerLockSynchronously([&](Worker::Lock& lock) {
      auto& isolateBase = jsg::IsolateBase::from(lock.getIsolate());
      KJ_EXPECT(isolateBase.isPreparingSnapshot());
      KJ_EXPECT(isolateBase.isStartingFromBaselineSnapshot());
      workerArtifact = isolateBase.extractSnapshotArtifact();
    });
  }
  KJ_EXPECT(workerArtifact->blob.data != nullptr && workerArtifact->blob.raw_size > 0);
  KJ_EXPECT(workerArtifact->bootstrapStateIndex != kj::none);

  // Step 2: Boot two independent request isolates from the worker snapshot. Both must observe the
  // same top-level `token`, counter == 1, working live-wrappable TextEncoder/TextDecoder in
  // URLSearchParams and EventSource, and absent Buffer/MessageChannel.
  auto runRestored = [&]() -> kj::String {
    TestFixture restored({
      .featureFlags = flags.asReader(),
      .mainModuleSource = workerSource,
      .snapshotConfig =
          jsg::SnapshotConfig(jsg::FinalizedSnapshot{.artifact = workerArtifact->addRef()}),
    });
    restored.enterWorkerLockSynchronously([](Worker::Lock& lock) {
      auto& isolateBase = jsg::IsolateBase::from(lock.getIsolate());
      KJ_EXPECT(isolateBase.isStartingFromSnapshot());
      KJ_EXPECT(!isolateBase.isStartingFromBaselineSnapshot());
    });
    auto response = restored.runRequest(kj::HttpMethod::GET, "http://example.com/", "");
    KJ_EXPECT(response.statusCode == 200);
    return kj::mv(response.body);
  };

  auto first = runRestored();
  auto second = runRestored();
  KJ_EXPECT(first == second, first, second);
  KJ_EXPECT(first.endsWith(":1:a=h%C3%A9llo&b=world:hi:false:false"_kj), first);
}

}  // namespace
}  // namespace workerd
