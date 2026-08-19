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

// An output stream wrapper recording the execution environment at every write and at
// end-of-stream (its destruction, for sinks that end by dropping the stream). Wrapping
// the test pipe's write end has a welcome side effect: it hides the pipe's own
// tryPumpFrom-based pump adoption, forcing the kj-level pump into its plain read/write
// loop, so both the reads AND the writes of the deferred phase are deterministically
// observed.
class ProbingOutputStream final: public kj::AsyncOutputStream {
 public:
  ProbingOutputStream(kj::Own<kj::AsyncOutputStream> inner, kj::Vector<EnvProbe>& probes)
      : inner(kj::mv(inner)),
        probes(probes) {}

  ~ProbingOutputStream() noexcept(false) {
    probes.add(captureEnv());
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    probes.add(captureEnv());
    return inner->write(buffer);
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    probes.add(captureEnv());
    return inner->write(pieces);
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return inner->whenWriteDisconnected();
  }

 private:
  kj::Own<kj::AsyncOutputStream> inner;
  kj::Vector<EnvProbe>& probes;
};

// Incrementally drains the far end of the test pipe once `start` resolves. Runs
// concurrently on the test's event loop; kj's one-way pipe is a rendezvous pipe, so the
// pump cannot make progress until this is pending (which the no-op tests exploit to park
// the flow).
kj::Promise<void> collectPipe(
    kj::Promise<void> start, kj::AsyncInputStream& in, kj::Vector<kj::byte>& received) {
  co_await start;
  auto buffer = kj::heapArray<kj::byte>(64);
  for (;;) {
    auto n = co_await in.tryRead(buffer.begin(), 1, buffer.size());
    if (n == 0) co_return;
    received.addAll(buffer.first(n));
  }
}

// Settles after n event loop turns.
kj::Promise<void> settleTurns(int n) {
  for (int i = 0; i < n; i++) {
    co_await kj::evalLater([]() {});
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
  auto readerTask = collectPipe(kj::READY_NOW, *pipe.in, received);

  auto gated = kj::heap<GatedInputStream>(kj::mv(gatePaf.promise), probes, deferredStart);
  auto probingOut = kj::heap<ProbingOutputStream>(kj::mv(pipe.out), probes);

  auto deferred = fixture.runInIoContext(
      [gated = kj::mv(gated), out = kj::mv(probingOut)](
          const TestFixture::Environment& env) mutable -> kj::Promise<DeferredProxy<void>> {
    // System source over the gated stream, system sink over the (probed) test pipe: the
    // system-to-system pump is the real deferred proxy production bodies use. Its
    // deferred segment is built from the RAW inner streams (see
    // EncodedAsyncOutputStream::tryPumpFrom) -- the wrapper methods that register
    // pending events on the IoContext are not part of it, which is what makes running it
    // after IoContext destruction legitimate.
    auto source = newSystemStream(kj::mv(gated), StreamEncoding::IDENTITY, env.context);
    auto sink = newSystemStream(kj::mv(out), StreamEncoding::IDENTITY, env.context);
    auto stream = JsReadableStream::create(env.js, env.context, kj::mv(source));
    return stream.pumpTo(env.js, kj::mv(sink), EndStream::YES);
  });

  // The fixture waited for the OUTER promise only, then destroyed the IncomingRequest
  // (the IoContext's sole owner). Holding the DeferredProxy here with the gate still
  // closed is itself the shape proof: a no-op proxy's outer promise resolves only at
  // EOF, so it would still be parked on the gated source and this point would never be
  // reached. (Verified by mutation: degrading tryPumpFrom to kj::none turns this test
  // into a timeout.)
  KJ_EXPECT(!IoContext::hasCurrent());
  KJ_EXPECT(v8::Isolate::TryGetCurrent() == nullptr);

  // Drive the pump to quiescence: the pre-gate bytes arrive and the pump parks on the
  // gate. (Whether the pre-gate transfer happened before or after the IoContext's
  // destruction is incidental -- the pump starts eagerly inside pumpTo(), so its first
  // read may run in-request. The post-destruction guarantees start at the gate.) Any
  // IoOwn tether left in the pump would trip its far-get check from here on.
  KJ_EXPECT(!deferred.proxyTask.poll(ws));
  KJ_EXPECT(received.asPtr() == kPreGate.asBytes());

  // Release the gate and finish the flow: the remaining reads, writes, and the sink's
  // end all run after IoContext destruction.
  gatePaf.fulfiller->fulfill();
  deferred.proxyTask.wait(ws);
  readerTask.wait(ws);
  KJ_EXPECT(received.size() == kPreGate.size() + kPostGate.size());
  KJ_EXPECT(received.asPtr().first(kPreGate.size()) == kPreGate.asBytes());
  KJ_EXPECT(received.asPtr().slice(kPreGate.size()) == kPostGate.asBytes());

  // Every deferred-phase operation -- the post-gate read, the post-gate write, the EOF
  // read(s), and the sink teardown -- ran with the isolate un-entered and no current
  // IoContext: the flow proceeded without pinning either. (Probes before deferredStart
  // are exempt: they belong to the eager in-request pump start.)
  KJ_EXPECT(deferredStart < probes.size());
  // At minimum: post-release read, post-gate write, EOF read, sink destruction.
  KJ_EXPECT(probes.size() - deferredStart >= 4);
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
// The opposite shape: for a no-op proxy, the OUTER promise includes the entire flow.
// The discriminating assertion runs in-request: with the far-end reader withheld, the
// flow parks on the rendezvous pipe and the outer promise must remain pending -- a real
// proxy's outer resolves at pump start regardless of the parked flow, so a change that
// silently converts one of these pumps into a real proxy fails the pending check
// loudly. (Merely polling the proxy task after the fact would not discriminate: driving
// a real proxy over small in-memory data completes it just as quickly.)

KJ_TEST("legacy pumpTo: a buffer-backed stream's proxy is a no-op") {
  TestFixture fixture;
  auto& ws = fixture.getWaitScope();

  auto readerGate = kj::newPromiseAndFulfiller<void>();
  auto pipe = kj::newOneWayPipe();
  kj::Vector<kj::byte> received;
  auto readerTask = collectPipe(kj::mv(readerGate.promise), *pipe.in, received);

  fixture.runInIoContext([&readerGate, out = kj::mv(pipe.out)](
                             const TestFixture::Environment& env) mutable -> kj::Promise<void> {
    // Memory-backed bodies must finish their I/O before the IoContext goes away, so
    // their pump defers nothing.
    auto sink = newSystemStream(kj::mv(out), StreamEncoding::IDENTITY, env.context);
    JsReadableStream stream(env.js, kj::str("hello world"));
    auto pump = stream.pumpTo(env.js, kj::mv(sink), EndStream::YES);

    bool outerResolved = false;
    // eagerlyEvaluate: a plain .then() chain is lazy (it only progresses under an
    // awaiter), which would make the pending check below pass vacuously. Eager
    // evaluation makes the chain self-driving so outer resolution is observed the
    // moment it happens.
    auto watched =
        pump.then([&outerResolved](DeferredProxy<void> proxy) mutable -> DeferredProxy<void> {
      outerResolved = true;
      return kj::mv(proxy);
    }).eagerlyEvaluate(nullptr);

    // With the reader withheld, the flow is parked on the pipe rendezvous. The outer
    // promise must still be pending: a no-op proxy's outer includes the flow.
    co_await settleTurns(10);
    KJ_EXPECT(!outerResolved);

    // Unpark the flow; the outer now completes, and the proxy task carries nothing.
    readerGate.fulfiller->fulfill();
    auto deferred = co_await watched;
    co_await deferred.proxyTask;
  });

  readerTask.wait(ws);
  KJ_EXPECT(received.asPtr() == "hello world"_kj.asBytes());
}

KJ_TEST("TS pumpTo: a queued stream's proxy is a no-op") {
  auto fixture = makeTsStreamsFixture();
  auto& ws = fixture.getWaitScope();

  auto readerGate = kj::newPromiseAndFulfiller<void>();
  auto pipe = kj::newOneWayPipe();
  kj::Vector<kj::byte> received;
  auto readerTask = collectPipe(kj::mv(readerGate.promise), *pipe.in, received);

  fixture.runInIoContext([&readerGate, out = kj::mv(pipe.out)](
                             const TestFixture::Environment& env) mutable -> kj::Promise<void> {
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
    auto pump = stream.pumpTo(env.js, kj::mv(sink), EndStream::YES);

    bool outerResolved = false;
    // eagerlyEvaluate: a plain .then() chain is lazy (it only progresses under an
    // awaiter), which would make the pending check below pass vacuously. Eager
    // evaluation makes the chain self-driving so outer resolution is observed the
    // moment it happens.
    auto watched =
        pump.then([&outerResolved](DeferredProxy<void> proxy) mutable -> DeferredProxy<void> {
      outerResolved = true;
      return kj::mv(proxy);
    }).eagerlyEvaluate(nullptr);

    co_await settleTurns(10);
    KJ_EXPECT(!outerResolved);

    readerGate.fulfiller->fulfill();
    auto deferred = co_await watched;
    co_await deferred.proxyTask;
  });

  readerTask.wait(ws);
  KJ_EXPECT(received.asPtr() == "hello world"_kj.asBytes());
}

}  // namespace
}  // namespace workerd::api
