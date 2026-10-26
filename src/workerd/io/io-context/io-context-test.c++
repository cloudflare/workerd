// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests the io-context crate inside and outside a request of a real worker. The Rust half is
// test_bridge.rs.

#include <workerd/io/io-context/ffi.h>
#include <workerd/io/io-context/test_bridge.rs.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/modules.h>
#include <workerd/rust/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/test.h>

namespace workerd::io_context::test {
namespace {

uintptr_t isolateOf(const TestFixture::V8Environment& env) {
  return reinterpret_cast<uintptr_t>(env.isolate);
}

int64_t nanosNow(IoContext& context) {
  return (context.now() - kj::UNIX_EPOCH) / kj::NANOSECONDS;
}

::rust::Slice<const uint8_t> slice(kj::ArrayPtr<const kj::byte> bytes) {
  return ::rust::Slice<const uint8_t>(bytes.begin(), bytes.size());
}

// A serialized message whose root is `text`.
kj::Array<kj::byte> serialize(kj::StringPtr text) {
  capnp::MallocMessageBuilder message;
  message.initRoot<capnp::AnyPointer>().setAs<capnp::Text>(text);
  auto words = capnp::messageToFlatArray(message);
  return kj::heapArray(words.asBytes());
}

// What the worker wrote to logfwdr, as "<channel>: <root text>" for each message.
struct LogfwdrLog {
  kj::Vector<kj::String> writes;
};

struct RecordingIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
  RecordingIoChannelFactory(TimerChannel& timer, LogfwdrLog& log)
      : DummyIoChannelFactory(timer),
        log(log) {}

  kj::Promise<void> writeLogfwdr(
      uint channel, kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage) override {
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<capnp::AnyPointer>();
    buildMessage(root);
    log.writes.add(kj::str(channel, ": ", root.getAs<capnp::Text>()));
    return kj::READY_NOW;
  }

  LogfwdrLog& log;
};

TestFixture::SetupParams recordingParams(
    LogfwdrLog& log, kj::Maybe<Worker::Actor::Id> actorId = kj::none) {
  return {
    .actorId = kj::mv(actorId),
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&log](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<RecordingIoChannelFactory>(timer, log);
  }),
  };
}

KJ_TEST("outside a request, current() fails with the error C++ IoContext::current() throws") {
  TestFixture fixture;
  fixture.enterWorkerLock([&](Worker::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](jsg::Lock& js) {
      KJ_EXPECT(!has_current(reinterpret_cast<uintptr_t>(js.v8Isolate)));
      auto rustMessage = current_error(reinterpret_cast<uintptr_t>(js.v8Isolate));
      js.tryCatch([&]() {
        IoContext::current();
        KJ_FAIL_EXPECT("IoContext::current() should throw outside a request");
      }, [&](jsg::Value exception) {
        auto message = jsg::JsValue(exception.getHandle(js)).toString(js);
        KJ_EXPECT(message == kj::str("Error: ", rustMessage.c_str()), message, rustMessage.c_str());
      });
    });
  });
}

KJ_TEST("inside a request, current() finds it, and now() is its clock") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(has_current(isolateOf(env)));
    KJ_EXPECT(current_error(isolateOf(env)) == ::rust::String());
    // The clock can advance between reads.
    auto before = nanosNow(env.context);
    auto now = now_nanos(isolateOf(env));
    KJ_EXPECT(before <= now && now <= nanosNow(env.context), before, now);
  });
}

KJ_TEST("write_logfwdr() writes the message to the channel") {
  LogfwdrLog log;
  TestFixture fixture(recordingParams(log));
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto message = serialize("hello");
    write_logfwdr(isolateOf(env), 7, slice(message));
  });
  KJ_ASSERT(log.writes.size() == 1);
  KJ_EXPECT(log.writes[0] == "7: hello");
}

KJ_TEST("write_logfwdr() rejects anything but exactly one message") {
  LogfwdrLog log;
  TestFixture fixture(recordingParams(log));
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto message = serialize("hello");
    KJ_EXPECT_THROW_MESSAGE(
        "the serialized message is empty", write_logfwdr(isolateOf(env), 7, slice(nullptr)));
    KJ_EXPECT_THROW_MESSAGE("not a whole number of words",
        write_logfwdr(isolateOf(env), 7, slice(message.first(message.size() - 1))));
    auto trailing = kj::heapArray<kj::byte>(message.size() + sizeof(capnp::word));
    trailing.first(message.size()).copyFrom(message);
    trailing.slice(message.size()).fill(0);
    KJ_EXPECT_THROW_MESSAGE(
        "bytes follow the serialized message", write_logfwdr(isolateOf(env), 7, slice(trailing)));
  });
  KJ_EXPECT(log.writes.size() == 0);
}

KJ_TEST("in a Durable Object, write_logfwdr() waits for the output gate") {
  LogfwdrLog log;
  TestFixture fixture(recordingParams(log, Worker::Actor::Id(kj::str("logfwdr-actor"))));
  auto gate = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(gate.promise), nullptr);

  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto message = serialize("after the gate");
    write_logfwdr(isolateOf(env), 3, slice(message));
  });
  fixture.pollEventLoop();
  KJ_EXPECT(log.writes.size() == 0);

  gate.fulfiller->fulfill();
  blocker.wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  KJ_ASSERT(log.writes.size() == 1);
  KJ_EXPECT(log.writes[0] == "3: after the gate");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("an IoContext id works only while its IoContext is current") {
  TestFixture fixture;
  auto first = fixture.newIncomingRequest();
  uint64_t firstId = 0;
  fixture.enterContext(*first, [&](const TestFixture::Environment& env) {
    firstId = io_context_id(env.context);
    auto before = nanosNow(env.context);
    auto now = io_context_now(firstId);
    KJ_EXPECT(before <= now && now <= nanosNow(env.context), before, now);
  });

  auto second = fixture.newIncomingRequest();
  fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
    KJ_EXPECT(io_context_id(env.context) != firstId);
    KJ_EXPECT_THROW_MESSAGE("not the one of the current request", io_context_now(firstId));
  });

  fixture.enterWorkerLockSynchronously([&](Worker::Lock&) {
    KJ_EXPECT_THROW_MESSAGE("there is no current request", io_context_now(firstId));
  });

  fixture.drainAndDestroy(kj::mv(second));
  fixture.drainAndDestroy(kj::mv(first));
}

constexpr kj::StringPtr PROBE_SPECIFIER = "test:io-context-probe"_kj;

KJ_TEST("the legacy registry creates a Rust module's exports outside the request resolving it") {
  // The type wrapper is used only to register C++ resources, which this test doesn't.
  struct UnusedTypeWrapper {};
  jsg::CompilationObserver observer;
  jsg::ModuleRegistryImpl<UnusedTypeWrapper> registry(observer);
  ::workerd::rust::jsg::RustModuleRegistry<decltype(registry), SuppressIoContextScope> adapter(
      registry);
  register_probe_module(adapter);

  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(registry.resolve(env.js, kj::Path::parse(PROBE_SPECIFIER), kj::none,
                  jsg::ModuleRegistry::ResolveOption::INTERNAL_ONLY) != kj::none);
    KJ_EXPECT(IoContext::hasCurrent());
  });
  KJ_EXPECT(take_probe_observation() == ProbeObservation::NoRequest);
}

KJ_TEST("the new registry creates a Rust module's exports outside the request resolving it") {
  constexpr auto mainModule = "export default {};"_kjc;
  const auto base = "file:///"_url;
  jsg::modules::ModuleRegistry::Builder registryBuilder(base);
  jsg::modules::ModuleBundle::BundleBuilder bundleBuilder(base);
  bundleBuilder.addEsmModule("main"_kj, jsg::copyToArc(mainModule));
  jsg::modules::ModuleBundle::BuiltinBuilder builtinBuilder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  ::workerd::rust::jsg::RustBuiltinModuleAdapter<SuppressIoContextScope> adapter(builtinBuilder);
  register_probe_module(adapter);
  registryBuilder.add(bundleBuilder.finish()).add(builtinBuilder.finish());

  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setNewModuleRegistry(true);
  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = mainModule,
    .useRealTimers = false,
    .newModuleRegistry = registryBuilder.finish(),
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(env.js.resolveInternalModule(PROBE_SPECIFIER) != kj::none);
    KJ_EXPECT(IoContext::hasCurrent());
  });
  KJ_EXPECT(take_probe_observation() == ProbeObservation::NoRequest);
}

}  // namespace
}  // namespace workerd::io_context::test
