// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Startup-snapshot copies of the binding objects an embedder may put in `env` that workerd's own
// config never does, so that no .wd-test reaches them: loopback Durable Object namespaces, an
// ExtendedFetcher, a Fetcher on a CrossContextOutgoingFactory, and a WrappedBinding. Each test
// copies an object as the zygote does (snapshotClone() on the original), then copies the copy as
// each Worker started from the snapshot does. See docs/startup-snapshot.md.

#include <workerd/api/export-loopback.h>
#include <workerd/api/http.h>
#include <workerd/api/wrapped-binding.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

WD_STRONG_BOOL(Copyable);

// The copies a test factory's cloneForSnapshot() made.
struct CopyCount {
  uint copies = 0;
};

class TestActorIdFactory final: public ActorIdFactory {
 public:
  TestActorIdFactory(Copyable copyable, CopyCount& count): copyable(copyable), count(count) {}

  kj::Own<ActorId> newUniqueId(kj::Maybe<kj::StringPtr> jurisdiction) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
  kj::Own<ActorId> idFromName(kj::String name) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
  kj::Own<ActorId> idFromString(kj::String str) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
  bool matchesJurisdiction(const ActorId& id) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
  kj::Own<ActorIdFactory> cloneWithJurisdiction(
      kj::Maybe<kj::StringPtr> maybeJurisdiction) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }

  kj::Maybe<kj::Own<ActorIdFactory>> cloneForSnapshot() const override {
    if (!copyable) return kj::none;
    ++count.copies;
    return kj::Own<ActorIdFactory>(kj::heap<TestActorIdFactory>(copyable, count));
  }

 private:
  Copyable copyable;
  CopyCount& count;
};

class TestCrossContextFactory final: public Fetcher::CrossContextOutgoingFactory {
 public:
  TestCrossContextFactory(Copyable copyable, CopyCount& count): copyable(copyable), count(count) {}

  Fetcher::OutgoingFactory::Result newSingleUseClient(IoContext& context,
      kj::Maybe<kj::String> cfStr,
      MakeUserSpanParent makeUserSpanParent) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }

  kj::Maybe<kj::Own<CrossContextOutgoingFactory>> cloneForSnapshot() const override {
    if (!copyable) return kj::none;
    ++count.copies;
    return kj::Own<CrossContextOutgoingFactory>(kj::heap<TestCrossContextFactory>(copyable, count));
  }

 private:
  Copyable copyable;
  CopyCount& count;
};

// A Fetcher subclass that does not override Fetcher::cloneForSnapshot(), as an embedder's might.
class TestSubclassFetcher final: public Fetcher {
 public:
  explicit TestSubclassFetcher(uint channel): Fetcher(channel, RequiresHostAndProtocol::YES) {}

  JSG_RESOURCE_TYPE(TestSubclassFetcher) {
    JSG_INHERIT(Fetcher);
  }
};

constexpr uint CHANNEL = 3;
constexpr uint CLASS_CHANNEL = 4;
constexpr uint16_t PORT = 5432;

// The copy a Worker started from the snapshot gets: a copy of the zygote's copy of `object`. None
// if either step refuses. Both copies must be of the original's type.
template <typename T>
kj::Maybe<kj::Own<jsg::Wrappable>> copyTwice(const T& object) {
  KJ_IF_SOME(stored, object.snapshotClone()) {
    KJ_ASSERT(stored->jsgGetMemoryName() == object.jsgGetMemoryName(), stored->jsgGetMemoryName());
    KJ_IF_SOME(started, stored->snapshotClone()) {
      KJ_ASSERT(
          started->jsgGetMemoryName() == object.jsgGetMemoryName(), started->jsgGetMemoryName());
      return kj::mv(started);
    }
  }
  return kj::none;
}

jsg::Ref<WrappedBinding> wrap(jsg::Lock& js, jsg::Ref<Fetcher> fetcher) {
  return js.alloc<WrappedBinding>(jsg::LenientOptional<jsg::Ref<Fetcher>>(kj::mv(fetcher)));
}

KJ_TEST("a Fetcher bound to a channel transfers, one with a connect override does not") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = env.js.alloc<Fetcher>(CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
    KJ_EXPECT(copyTwice(*fetcher) != kj::none);
    KJ_EXPECT(fetcher->cloneForSnapshot() != kj::none);

    fetcher->mapVirtualHost(env.js, PORT);
    KJ_EXPECT(fetcher->snapshotClone() == kj::none);
    KJ_EXPECT(fetcher->cloneForSnapshot() == kj::none);
  });
}

KJ_TEST("an ExtendedFetcher transfers with its port until its host is read") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher =
        env.js.alloc<ExtendedFetcher>(CHANNEL, Fetcher::RequiresHostAndProtocol::YES, PORT);
    KJ_EXPECT(copyTwice(*fetcher) != kj::none);
    auto refCopy = KJ_ASSERT_NONNULL(fetcher->cloneForSnapshot());
    KJ_EXPECT(kj::downcast<ExtendedFetcher>(*refCopy).getPort() == PORT);

    // Reading `host` registers the connect override on this global scope.
    fetcher->getHost(env.js);
    KJ_EXPECT(fetcher->snapshotClone() == kj::none);
    KJ_EXPECT(fetcher->cloneForSnapshot() == kj::none);
  });
}

KJ_TEST("a Fetcher on a cross-context factory transfers if the factory copies itself") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    CopyCount count;
    auto fetcher =
        env.js.alloc<Fetcher>(kj::Own<Fetcher::CrossContextOutgoingFactory>(
                                  kj::heap<TestCrossContextFactory>(Copyable::YES, count)),
            Fetcher::RequiresHostAndProtocol::YES);
    KJ_EXPECT(copyTwice(*fetcher) != kj::none);
    KJ_EXPECT(count.copies == 2);

    auto uncopyable =
        env.js.alloc<Fetcher>(kj::Own<Fetcher::CrossContextOutgoingFactory>(
                                  kj::heap<TestCrossContextFactory>(Copyable::NO, count)),
            Fetcher::RequiresHostAndProtocol::YES);
    KJ_EXPECT(uncopyable->snapshotClone() == kj::none);
    KJ_EXPECT(uncopyable->cloneForSnapshot() == kj::none);

    // A cross-context Fetcher that registered a connect override does not transfer either.
    fetcher->mapVirtualHost(env.js, PORT);
    KJ_EXPECT(fetcher->snapshotClone() == kj::none);
  });
}

KJ_TEST("a WrappedBinding transfers with a copy of its fetcher") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto empty = env.js.alloc<WrappedBinding>(jsg::LenientOptional<jsg::Ref<Fetcher>>(kj::none));
    KJ_EXPECT(copyTwice(*empty) != kj::none);

    auto plain =
        wrap(env.js, env.js.alloc<Fetcher>(CHANNEL, Fetcher::RequiresHostAndProtocol::YES));
    KJ_EXPECT(copyTwice(*plain) != kj::none);

    auto extended = wrap(env.js,
        env.js.alloc<ExtendedFetcher>(CHANNEL, Fetcher::RequiresHostAndProtocol::YES, PORT));
    KJ_EXPECT(copyTwice(*extended) != kj::none);
  });
}

KJ_TEST("a WrappedBinding whose fetcher cannot transfer as itself does not transfer") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    // The subclass copies as a plain Fetcher, which the WrappedBinding refuses.
    auto subclass = wrap(env.js, env.js.alloc<TestSubclassFetcher>(CHANNEL));
    KJ_EXPECT(subclass->snapshotClone() == kj::none);

    auto overridden = env.js.alloc<Fetcher>(CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
    overridden->mapVirtualHost(env.js, PORT);
    KJ_EXPECT(wrap(env.js, kj::mv(overridden))->snapshotClone() == kj::none);
  });
}

KJ_TEST("a loopback Durable Object namespace transfers if its ID factory copies itself") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    CopyCount count;
    auto ns = env.js.alloc<LoopbackDurableObjectNamespace>(CHANNEL,
        kj::Own<ActorIdFactory>(kj::heap<TestActorIdFactory>(Copyable::YES, count)),
        ActorCallRetriesAllowed::YES, env.js.alloc<LoopbackDurableObjectClass>(CLASS_CHANNEL),
        Persistent::NO);
    KJ_EXPECT(copyTwice(*ns) != kj::none);
    KJ_EXPECT(count.copies == 2);

    auto uncopyable = env.js.alloc<LoopbackDurableObjectNamespace>(CHANNEL,
        kj::Own<ActorIdFactory>(kj::heap<TestActorIdFactory>(Copyable::NO, count)),
        ActorCallRetriesAllowed::YES, env.js.alloc<LoopbackDurableObjectClass>(CLASS_CHANNEL),
        Persistent::NO);
    KJ_EXPECT(uncopyable->snapshotClone() == kj::none);
  });
}

KJ_TEST("a loopback colo-local actor namespace transfers") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto ns = env.js.alloc<LoopbackColoLocalActorNamespace>(
        CHANNEL, env.js.alloc<LoopbackDurableObjectClass>(CLASS_CHANNEL));
    KJ_EXPECT(copyTwice(*ns) != kj::none);
  });
}

}  // namespace
}  // namespace workerd::api
