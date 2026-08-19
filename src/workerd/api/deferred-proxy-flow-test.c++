// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Proof tests for the deferred-proxy PROPERTY (api/deferred-proxy.h): when a pump's
// deferred proxy is not a no-op, the pump's outer promise resolves at pump start and the
// entire data flow happens in the deferred phase -- with the isolate un-entered, no
// current IoContext, and the IoContext itself already destroyed. This mirrors the
// production shape in io/worker-entrypoint.c++ Stage 3, which awaits the proxy task only
// after dropping the incoming request ("we can finish proxying without pinning it or the
// isolate into memory").
//
// The choreography leans on a TestFixture affordance: runInIoContext() unwraps exactly
// one promise layer from the callback's return type, so a callback returning
// kj::Promise<DeferredProxy<void>> makes the fixture wait for the OUTER promise only and
// hand the DeferredProxy back by value AFTER the IncomingRequest -- the IoContext's sole
// owner -- has been destroyed. Driving proxyTask from there reproduces Stage 3 exactly.
//
// The real (non-no-op) proxy under test is the system-to-system pump
// (EncodedAsyncOutputStream::tryPumpFrom), the path production response bodies take.
// The companion no-op tests pin the opposite shape -- outer resolves only at EOF -- so a
// change that silently flips a pump between the two shapes fails loudly.

#include <workerd/api/js-readable-stream.h>
#include <workerd/api/js-streams-bridge.h>
#include <workerd/api/system-streams.h>
#include <workerd/io/io-context.h>
#include <workerd/io/per-isolate-bootstrap.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/async-io.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

constexpr kj::StringPtr kPreGate = "hello "_kj;
constexpr kj::StringPtr kPostGate = "world"_kj;

// The execution environment observed inside a stream operation.
struct EnvProbe {
  bool isolateEntered;
  bool ioContextCurrent;
};

EnvProbe captureEnv() {
  return {
    .isolateEntered = v8::Isolate::TryGetCurrent() != nullptr,
    .ioContextCurrent = IoContext::hasCurrent(),
  };
}

// A test-owned (never IoContext-owned) input stream serving kPreGate, then blocking on
// the gate, then serving kPostGate, then EOF (idempotently -- the system pump performs an
// extra EOF-verify read). Records the execution environment at every read entry and again
// immediately after the gate releases.
//
// The recorded probes split into two groups. KJ coroutines run eagerly until their first
// suspension, so the pump's first read(s) typically execute synchronously inside the
// pumpTo() call itself -- under the Worker lock, with the IoContext current. That is fine
// (the flow does not REQUIRE that environment; it merely started there). The property
// under test concerns everything from `deferredStart` on: those probes are recorded at or
// after the gate release, which the test performs only after the IoContext has been
// destroyed, so they must all observe a clean environment.
class GatedInputStream final: public kj::AsyncInputStream {
 public:
  GatedInputStream(kj::Promise<void> gate, kj::Vector<EnvProbe>& probes, size_t& deferredStart)
      : gate(kj::mv(gate)),
        probes(probes),
        deferredStart(deferredStart) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    probes.add(captureEnv());
    switch (phase) {
      case Phase::PRE: {
        phase = Phase::GATED;
        KJ_ASSERT(maxBytes >= kPreGate.size());
        kj::arrayPtr(static_cast<kj::byte*>(buffer), kPreGate.size()).copyFrom(kPreGate.asBytes());
        co_return kPreGate.size();
      }
      case Phase::GATED: {
        phase = Phase::DONE;
        co_await gate;
        // Everything from here on ran strictly after the test released the gate.
        deferredStart = kj::min(deferredStart, probes.size());
        probes.add(captureEnv());
        KJ_ASSERT(maxBytes >= kPostGate.size());
        kj::arrayPtr(static_cast<kj::byte*>(buffer), kPostGate.size())
            .copyFrom(kPostGate.asBytes());
        co_return kPostGate.size();
      }
      case Phase::DONE: {
        co_return 0;
      }
    }
    KJ_UNREACHABLE;
  }

 private:
  enum class Phase { PRE, GATED, DONE };
  Phase phase = Phase::PRE;
  kj::Promise<void> gate;
  kj::Vector<EnvProbe>& probes;
  size_t& deferredStart;
};

// Incrementally drains the far end of the test pipe. Runs concurrently on the test's
// event loop; kj's one-way pipe is a rendezvous pipe, so the pump cannot make progress
// unless this is pending.
kj::Promise<void> collectPipe(kj::AsyncInputStream& in, kj::Vector<kj::byte>& received) {
  auto buffer = kj::heapArray<kj::byte>(64);
  for (;;) {
    auto n = co_await in.tryRead(buffer.begin(), 1, buffer.size());
    if (n == 0) co_return;
    received.addAll(buffer.first(n));
  }
}

TestFixture makeTsStreamsFixture() {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setTypeScriptImplementedStreams(true);
  return TestFixture({
    .featureFlags = flags.asReader(),
    .autogates = kj::arr("per-isolate-javascript-bootstrap"_kj),
  });
}

// The core proof, shared by the legacy and TypeScript backends: pump a gated system
// source into a system sink, receive the DeferredProxy after the IoContext is destroyed,
// and verify the whole flow happens in the deferred phase with a clean environment.
void runRealProxyProof(TestFixture& fixture) {
  auto& ws = fixture.getWaitScope();

  auto gatePaf = kj::newPromiseAndFulfiller<void>();
  kj::Vector<EnvProbe> probes;
  size_t deferredStart = kj::maxValue;
  auto pipe = kj::newOneWayPipe();
  kj::Vector<kj::byte> received;
  auto readerTask = collectPipe(*pipe.in, received);

  auto gated = kj::heap<GatedInputStream>(kj::mv(gatePaf.promise), probes, deferredStart);

  auto deferred = fixture.runInIoContext(
      [gated = kj::mv(gated), out = kj::mv(pipe.out)](
          const TestFixture::Environment& env) mutable -> kj::Promise<DeferredProxy<void>> {
    // System source over the gated stream, system sink over the test pipe: the
    // system-to-system pump is the real deferred proxy production bodies use.
    auto source = newSystemStream(kj::mv(gated), StreamEncoding::IDENTITY, env.context);
    auto sink = newSystemStream(kj::mv(out), StreamEncoding::IDENTITY, env.context);
    auto stream = JsReadableStream::create(env.js, env.context, kj::mv(source));
    return stream.pumpTo(env.js, kj::mv(sink), EndStream::YES);
  });

  // The fixture waited for the OUTER promise only, then destroyed the IncomingRequest
  // (the IoContext's sole owner). Holding the DeferredProxy here with the gate still
  // closed is itself the shape proof: a no-op proxy's outer promise resolves only at
  // EOF, so it would still be parked on the gated source and this point would never be
  // reached.
  KJ_EXPECT(!IoContext::hasCurrent());
  KJ_EXPECT(v8::Isolate::TryGetCurrent() == nullptr);

  // Drive the deferred phase up to the gate: the pre-gate bytes flow with the IoContext
  // already gone. (Any IoOwn tether left in the pump would trip its far-get check here.)
  KJ_EXPECT(!deferred.proxyTask.poll(ws));
  KJ_EXPECT(received.asPtr() == kPreGate.asBytes());

  // Release the gate and finish the flow, still with no IoContext and no isolate.
  gatePaf.fulfiller->fulfill();
  deferred.proxyTask.wait(ws);
  readerTask.wait(ws);
  KJ_EXPECT(received.size() == kPreGate.size() + kPostGate.size());
  KJ_EXPECT(received.asPtr().first(kPreGate.size()) == kPreGate.asBytes());
  KJ_EXPECT(received.asPtr().slice(kPreGate.size()) == kPostGate.asBytes());

  // Every deferred-phase operation -- the post-gate delivery and the EOF read(s) -- ran
  // with the isolate un-entered and no current IoContext: the flow proceeded without
  // pinning either. (Earlier probes are incidental: the pump coroutine starts eagerly
  // inside the pumpTo() call, so the first read typically runs under the Worker lock.)
  KJ_EXPECT(deferredStart < probes.size());
  KJ_EXPECT(probes.size() - deferredStart >= 2);  // post-release delivery + EOF read
  for (auto& probe: probes.asPtr().slice(deferredStart)) {
    KJ_EXPECT(!probe.isolateEntered);
    KJ_EXPECT(!probe.ioContextCurrent);
  }
}

KJ_TEST("legacy pumpTo: a real deferred proxy flows entirely after IoContext destruction") {
  TestFixture fixture;
  runRealProxyProof(fixture);
}

KJ_TEST("TS pumpTo: a real deferred proxy flows entirely after IoContext destruction") {
  // Under the flag the same choreography goes through the TypeScript stream, the
  // kExtractNativeSource extraction, releaseForPump, and pumpExtractedSource -- proving
  // the extraction plumbing fully de-tethers the source from the IoContext.
  auto fixture = makeTsStreamsFixture();
  runRealProxyProof(fixture);
}

// ---------------------------------------------------------------------------
// The opposite shape: no-op proxies complete their entire flow before the outer promise
// resolves. These pins keep the classification honest -- a change that silently converts
// a real proxy into a no-op (or vice versa) must fail one of these suites.

KJ_TEST("legacy pumpTo: a buffer-backed stream's proxy is a no-op") {
  TestFixture fixture;
  auto& ws = fixture.getWaitScope();

  auto pipe = kj::newOneWayPipe();
  kj::Vector<kj::byte> received;
  auto readerTask = collectPipe(*pipe.in, received);

  auto deferred = fixture.runInIoContext(
      [out = kj::mv(pipe.out)](
          const TestFixture::Environment& env) mutable -> kj::Promise<DeferredProxy<void>> {
    // Memory-backed bodies must finish their I/O before the IoContext goes away, so
    // their pump defers nothing.
    auto sink = newSystemStream(kj::mv(out), StreamEncoding::IDENTITY, env.context);
    JsReadableStream stream(env.js, kj::str("hello world"));
    return stream.pumpTo(env.js, kj::mv(sink), EndStream::YES);
  });

  // No-op shape: by the time the outer promise resolved, the flow already completed;
  // the proxy task has nothing left to do.
  KJ_EXPECT(deferred.proxyTask.poll(ws));
  deferred.proxyTask.wait(ws);
  readerTask.wait(ws);
  KJ_EXPECT(received.asPtr() == "hello world"_kj.asBytes());
}

KJ_TEST("TS pumpTo: a queued stream's proxy is a no-op") {
  auto fixture = makeTsStreamsFixture();
  auto& ws = fixture.getWaitScope();

  auto pipe = kj::newOneWayPipe();
  kj::Vector<kj::byte> received;
  auto readerTask = collectPipe(*pipe.in, received);

  auto deferred = fixture.runInIoContext(
      [out = kj::mv(pipe.out)](
          const TestFixture::Environment& env) mutable -> kj::Promise<DeferredProxy<void>> {
    auto& js = env.js;
    // A queued (plain JS underlying source) stream: the pump drives the JS conduit
    // under the isolate lock, so nothing can be deferred.
    auto underlying = js.obj();
    underlying.set(js, "start"_kj,
        jsg::JsValue(js.wrapSimpleFunction(
            js.v8Context(), [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) {
      auto controller = KJ_ASSERT_NONNULL(jsg::JsValue(info[0]).tryCast<jsg::JsObject>());
      auto enqueue = KJ_ASSERT_NONNULL(controller.get(js, "enqueue"_kj).tryCast<jsg::JsFunction>());
      enqueue.call(js, controller, jsg::JsUint8Array::create(js, "hello world"_kj.asBytes()));
      auto close = KJ_ASSERT_NONNULL(controller.get(js, "close"_kj).tryCast<jsg::JsFunction>());
      close.call(js, controller);
    })));
    auto constructor = webstreams::getCppExport(js, "ReadableStream");
    auto stream =
        JsReadableStream(js, constructor.newInstance(js, jsg::JsValue(underlying)).addRef(js));
    auto sink = newSystemStream(kj::mv(out), StreamEncoding::IDENTITY, env.context);
    return stream.pumpTo(env.js, kj::mv(sink), EndStream::YES);
  });

  KJ_EXPECT(deferred.proxyTask.poll(ws));
  deferred.proxyTask.wait(ws);
  readerTask.wait(ws);
  KJ_EXPECT(received.asPtr() == "hello world"_kj.asBytes());
}

}  // namespace
}  // namespace workerd::api
