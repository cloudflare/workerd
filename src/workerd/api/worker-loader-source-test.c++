// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests of how WorkerLoader::get() hands the DynamicWorkerSource that getCode() describes to the
// runtime that loads the Worker.

#include <workerd/api/http.h>
#include <workerd/api/worker-loader.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

// Records its destruction. WorkerLoader makes it the loaded Worker's global outbound, so it lives
// exactly as long as the DynamicWorkerSource that holds it.
class TrackedChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  explicit TrackedChannel(bool& destroyed): destroyed(destroyed) {}
  ~TrackedChannel() noexcept(false) {
    destroyed = true;
  }

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata) override {
    KJ_UNREACHABLE;
  }
  void requireAllowsTransfer() override {}
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage) override {
    KJ_UNREACHABLE;
  }

 private:
  bool& destroyed;
};

class NeverUsedStubChannel final: public WorkerStubChannel {
 public:
  kj::Rc<IoChannelFactory::SubrequestChannel> getEntrypointResolved(
      kj::Maybe<kj::String>, Frankenvalue, kj::Maybe<ResourceLimits>) override {
    KJ_UNREACHABLE;
  }
  kj::Rc<IoChannelFactory::ActorClassChannel> getActorClassResolved(
      kj::Maybe<kj::String>, Frankenvalue, kj::Maybe<ResourceLimits>) override {
    KJ_UNREACHABLE;
  }
};

// What the loader channel factory shares with the test.
struct LoaderState {
  bool channelDestroyed = false;
  kj::Maybe<kj::Function<kj::Promise<DynamicWorkerSource>()>> fetchSource;
};

// Keeps the source callback that WorkerLoader::get() passes to loadIsolate(), so the test can
// call it as the runtime would when it first needs the Worker.
struct LoaderChannelFactory final: public TestFixture::DummyIoChannelFactory {
  LoaderChannelFactory(TimerChannel& timer, LoaderState& state)
      : DummyIoChannelFactory(timer),
        state(state) {}

  kj::Rc<SubrequestChannel> getSubrequestChannelResolved(
      uint channel, kj::Maybe<Frankenvalue>, kj::Maybe<VersionRequest>, Persistent) override {
    KJ_ASSERT(channel == IoContext::NULL_CLIENT_CHANNEL);
    return kj::rc<TrackedChannel>(state.channelDestroyed);
  }

  kj::Own<WorkerStubChannel> loadIsolate(uint,
      kj::Maybe<kj::String>,
      kj::Function<kj::Promise<DynamicWorkerSource>()> fetchSource) override {
    state.fetchSource = kj::mv(fetchSource);
    return kj::refcounted<NeverUsedStubChannel>();
  }

  LoaderState& state;
};

WorkerLoader::WorkerCode makeWorkerCode() {
  auto modules = kj::heapArrayBuilder<jsg::Dict<
      kj::OneOf<jsg::V8Ref<v8::WasmModuleObject>, WorkerLoader::Module, kj::String>>::Field>(1);
  modules.add(kj::str("main.js"), kj::str("export default {}"));
  return {
    .compatibilityDate = kj::str("2024-01-01"),
    .mainModule = kj::str("main.js"),
    .modules = {.fields = modules.finish()},
  };
}

KJ_TEST("WorkerLoader::get() hands the source to KJ in the microtask that converts it") {
  // A limit can terminate execution between two microtasks, discarding the rest of the queue. The
  // microtask that converts the WorkerCode into a DynamicWorkerSource must also hand the source to
  // the KJ promise returned to the runtime. Were the handoff left to another microtask, it would
  // be discarded: the runtime's wait would never settle, and the source, with the KJ channels it
  // holds, would stay parked in the V8 heap.
  LoaderState state;
  TestFixture fixture(TestFixture::SetupParams{
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&state](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<LoaderChannelFactory>(timer, state);
  }),
  });
  auto request = fixture.newIncomingRequest();
  auto& context = request->getContext();

  context
      .run([&](Worker::Lock& lock) {
    auto& js = jsg::Lock::from(lock.getIsolate());
    auto loader = js.alloc<WorkerLoader>(0, CompatibilityDateValidation::CODE_VERSION);
    loader->get(
        js, kj::none, jsg::Function<jsg::Promise<WorkerLoader::WorkerCode>()>([](jsg::Lock& js) {
      // Terminates execution in the second round of microtasks: after the microtask that the
      // WorkerCode's resolution schedules, and before any microtask that one schedules in turn.
      js.resolvedPromise().then(js, [](jsg::Lock&) {}).then(js, [](jsg::Lock& js) {
        js.terminateExecutionNow();
      });
      return js.resolvedPromise(makeWorkerCode());
    }));
  }).wait(fixture.getWaitScope());

  auto promise = KJ_ASSERT_NONNULL(state.fetchSource)();
  fixture.pollEventLoop();
  // Were the handoff left to a discarded microtask, the wait would never settle, and the source
  // would stay in the V8 heap.
  KJ_ASSERT(promise.poll(fixture.getWaitScope()), "the termination discarded the source");
  {
    auto source = promise.wait(fixture.getWaitScope());
    KJ_EXPECT(source.globalOutbound != kj::none);
    KJ_EXPECT(!state.channelDestroyed);
  }
  KJ_EXPECT(state.channelDestroyed);

  state.fetchSource = kj::none;
  fixture.drainAndDestroy(kj::mv(request));
}

}  // namespace
}  // namespace workerd::api
