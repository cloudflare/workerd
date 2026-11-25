// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The behaviour both per-isolate bootstrap loaders share, run once with each: the C++ loader,
// and the Rust one that the RUST_PER_ISOLATE_BOOTSTRAP autogate selects.

#include <workerd/api/per-isolate-bootstrap.h>
#include <workerd/jsg/snapshot.h>
#include <workerd/tests/test-fixture.h>

#include <embedder_bootstrap_test/embedder_bootstrap_test.capnp.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd {
namespace {

using jsg::BootstrapLoader;

constexpr BootstrapLoader LOADERS[] = {BootstrapLoader::CPP, BootstrapLoader::RUST};

kj::StringPtr loaderName(BootstrapLoader loader) {
  return loader == BootstrapLoader::RUST ? "rust"_kj : "c++"_kj;
}

// The autogates under which a new context gets `loader`.
kj::Array<util::AutogateKey> gatesFor(BootstrapLoader loader) {
  if (loader == BootstrapLoader::RUST) {
    return kj::arr(util::AutogateKey::RUST_PER_ISOLATE_BOOTSTRAP);
  }
  return nullptr;
}

jsg::JsObject getExports(jsg::Lock& js, kj::StringPtr specifier) {
  auto exports = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, specifier), specifier);
  return KJ_ASSERT_NONNULL(exports.tryCast<jsg::JsObject>(), specifier);
}

// Whether `fn` throws a JavaScript exception.
bool throws(jsg::Lock& js, kj::FunctionParam<void()> fn) {
  bool threw = false;
  js.tryCatch([&] { fn(); }, [&](jsg::Value) { threw = true; });
  return threw;
}

KJ_TEST("require() refuses unknown scripts, the embedder's from workerd's scope, and cycles") {
  EmbedderBootstrap embedder(EMBEDDER_BOOTSTRAP_TEST_BUNDLE, "test-embedder/");
  for (auto loader: LOADERS) {
    KJ_CONTEXT(loaderName(loader));
    TestFixture fixture({
      .autogateKeys = gatesFor(loader),
      .ignoreAllAutogatesEnv = true,
      .embedderBootstrap = embedder,
    });
    fixture.runInIoContext([](const TestFixture::Environment& env) {
      auto& js = env.js;
      {
        KJ_EXPECT_LOG(ERROR, "per-isolate bootstrap: unknown script");
        KJ_EXPECT(throws(js, [&] { std::ignore = requireBootstrapExport(js, "no-such-script"); }));
      }

      // requireBootstrapExport() requires as workerd's scripts do, which cannot load the
      // embedder's.
      {
        KJ_EXPECT_LOG(ERROR, "workerd's scripts cannot require the embedder's");
        KJ_EXPECT(
            throws(js, [&] { std::ignore = requireBootstrapExport(js, "test-embedder/cycle-a"); }));
      }
      KJ_EXPECT(tryGetBootstrapExport(js, "test-embedder/cycle-a") == kj::none);

      // A cycle fails each require() that enters it, and leaves nothing loading or cached.
      auto requireLater = KJ_ASSERT_NONNULL(
          getExports(js, "test-embedder/main").get(js, "requireLater").tryCast<jsg::JsFunction>());
      for (auto attempt: {1, 2}) {
        KJ_CONTEXT(attempt);
        KJ_EXPECT_LOG(ERROR, "circular dependency in per-isolate bootstrap");
        KJ_EXPECT(throws(js, [&] {
          std::ignore = requireLater.call(js, js.undefined(), js.str("test-embedder/cycle-a"_kj));
        }));
      }
      KJ_EXPECT(tryGetBootstrapExport(js, "test-embedder/cycle-a") == kj::none);
      KJ_EXPECT(tryGetBootstrapExport(js, "test-embedder/cycle-b") == kj::none);
    });
  }
}

// A Worker whose top level reads the lazy MessageChannel global, which requires
// messagechannel/messagechannel after the bootstrap.
constexpr kj::StringPtr READS_LAZY_GLOBAL = R"SCRIPT(
  const channel = MessageChannel;
  export default {
    fetch(request) { return new Response(typeof channel); },
  };
)SCRIPT"_kj;

// Snapshots a zygote that runs the `zygote` loader, then starts a Worker from the snapshot under
// the autogates that select `restored`. The Worker keeps the zygote's loader and its require()
// cache, including what the top level required after the bootstrap when `topLevelReads`.
void testRestore(BootstrapLoader zygote, BootstrapLoader restored, bool topLevelReads) {
  KJ_CONTEXT(loaderName(zygote), loaderName(restored), topLevelReads);
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setExposeGlobalMessageChannel(true);
  kj::Maybe<kj::StringPtr> mainModule;
  if (topLevelReads) {
    mainModule = READS_LAZY_GLOBAL;
  }

  kj::Own<jsg::SnapshotArtifact> artifact;
  {
    TestFixture fixture({
      .featureFlags = flags.asReader(),
      .autogateKeys = gatesFor(zygote),
      .ignoreAllAutogatesEnv = true,
      .mainModuleSource = mainModule,
      .snapshotConfig = jsg::SnapshotConfig(
          jsg::MutableSnapshot{.artifact = kj::atomicRefcounted<jsg::SnapshotArtifact>()}),
    });
    artifact = fixture.extractSnapshotArtifact();
  }
  auto& pinned = KJ_ASSERT_NONNULL(artifact->bootstrapState);
  KJ_EXPECT(pinned.loader == zygote);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .autogateKeys = gatesFor(restored),
    .ignoreAllAutogatesEnv = true,
    .mainModuleSource = mainModule,
    .snapshotConfig = jsg::SnapshotConfig(jsg::FinalizedSnapshot{.artifact = artifact->addRef()}),
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    KJ_EXPECT(tryGetBootstrapExport(js, "main") != kj::none);
    KJ_EXPECT(
        (tryGetBootstrapExport(js, "messagechannel/messagechannel") != kj::none) == topLevelReads);
    // Requires the module now unless the snapshot has it.
    auto channel = js.global().get(js, "MessageChannel"_kj);
    KJ_EXPECT(channel.strictEquals(
        getExports(js, "messagechannel/messagechannel").get(js, "MessageChannel"_kj)));
  });
}

KJ_TEST("a Worker started from a snapshot keeps the zygote's loader and require() cache") {
  for (auto zygote: LOADERS) {
    for (auto restored: LOADERS) {
      for (bool topLevelReads: {false, true}) {
        testRestore(zygote, restored, topLevelReads);
      }
    }
  }
}

}  // namespace
}  // namespace workerd
