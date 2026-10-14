// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Termination injection for the streams implementation.
//
// When a Worker exceeds its CPU limit, V8 terminates execution: the next call into JavaScript
// fails, and the failure unwinds through whatever C++ was calling it. JSG_CATCH blocks do not run
// for termination. The isolate is then reused, so any stream left half-updated by the unwind can
// still be reached by queued promise continuations, later microtasks, and the garbage collector.
//
// Each scenario below is run once to count how many times it calls from C++ into JavaScript,
// then once more for every one of those calls, terminating execution at it. After each
// terminated run the test cancels the termination, pokes every stream and controller the
// scenario created, runs the remaining microtasks, and forces a full GC. Any crash, KJ assertion,
// C++ exception surfacing as a JavaScript error, or run that never finishes in those steps is a
// stream whose invariants did not survive the unwind. Leaks show up when this runs under ASAN.

#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/mutex.h>
#include <kj/test.h>
#include <kj/thread.h>

#include <atomic>

namespace workerd::api {
namespace {

// Counts calls from C++ into JavaScript and terminates execution at a chosen one.
struct TerminationInjector {
  static inline uint64_t calls = 0;
  // 0 means never terminate.
  static inline uint64_t terminateAt = 0;
  // Whether the termination was injected inside a microtask checkpoint. V8 clears a termination
  // that reaches the end of a checkpoint with no JavaScript on the stack, so it is gone
  // afterwards without anything having swallowed it.
  static inline bool injectedInMicrotask = false;

  static void onBeforeCallEntered(v8::Isolate* isolate) {
    if (++calls == terminateAt) {
      injectedInMicrotask = v8::MicrotasksScope::IsRunningMicrotasks(isolate);
      isolate->TerminateExecution();
    }
  }

  static void arm(uint64_t at) {
    calls = 0;
    terminateAt = at;
    injectedInMicrotask = false;
  }
};

// Upper bound on the injection points tried per scenario, to keep the test's runtime bounded.
constexpr uint64_t MAX_INJECTION_POINTS = 3000;

constexpr kj::StringPtr SCENARIOS = R"SCRIPT(
const live = [];
// Errors that came from a C++ exception rather than from JavaScript. Those are bugs, so the C++
// side of the test fails on them instead of letting them pass as ordinary rejections.
const internalErrors = [];
let pending = [];
const keep = (...xs) => { live.push(...xs); return xs[0]; };
const check = (e) => {
  if (e instanceof Error && e.message.startsWith('internal error')) internalErrors.push(String(e.stack));
};
const ignore = (p) => { if (p && typeof p.then === 'function') p.then(() => {}, check); return p; };
const work = () => [1, 2, 3].map((x) => x * 2).reduce((a, b) => a + b);
const tick = (n = 2) => { let p = Promise.resolve(); for (let i = 0; i < n; i++) p = p.then(() => {}); return p; };

function source(n, extra = {}) {
  let i = 0;
  return keep(new ReadableStream({
    start(c) { keep(c); if (extra.start) extra.start(c); },
    pull(c) {
      work();
      if (extra.pull) return extra.pull(c, i++);
      if (i < n) c.enqueue(`chunk${i++}`); else c.close();
    },
    cancel(reason) { work(); if (extra.cancel) return extra.cancel(reason); },
  }, { highWaterMark: extra.highWaterMark ?? 1 }));
}

function sink(extra = {}) {
  return keep(new WritableStream({
    start(c) { keep(c); if (extra.start) extra.start(c); },
    write(chunk, c) { work(); if (extra.write) return extra.write(chunk, c); },
    close() { work(); if (extra.close) return extra.close(); },
    abort(reason) { work(); if (extra.abort) return extra.abort(reason); },
  }, { highWaterMark: extra.highWaterMark ?? 1 }));
}

async function readAll(rs) {
  const reader = keep(rs.getReader());
  for (;;) {
    const { done } = await reader.read();
    if (done) return;
  }
}

const stalled = (extra = {}) => source(0, { ...extra, pull() {} });

// Each scenario sets up some streams and returns its later steps, if any. The test calls every
// step directly from C++, draining microtasks between steps. That matters: when execution is
// terminated inside a microtask, V8 discards the rest of the microtask queue, so teardown that
// only ever runs from a promise reaction would hide continuations that survive in production.
const scenarios = [
  ['pipeTo', () => { ignore(source(3).pipeTo(sink())); }],
  ['pipeTo-async-sink', () => { ignore(source(3).pipeTo(sink({ write: () => tick() }))); }],
  ['pipeTo-destination-errors-in-write', () => {
    let wc;
    ignore(source(5).pipeTo(sink({ start(c) { wc = c; }, write() { wc.error(new Error('sink')); } })));
  }],
  ['pipeTo-destination-errors-while-reading', () => {
    let wc;
    ignore(stalled().pipeTo(sink({ start(c) { wc = c; } })));
    return [() => wc.error(new Error('sink'))];
  }],
  ['pipeTo-destination-closed-while-reading', () => {
    const ws = sink({ close: () => tick() });
    const writer = ws.getWriter();
    ignore(writer.close());
    writer.releaseLock();
    ignore(stalled().pipeTo(ws));
  }],
  ['pipeTo-destination-aborted-while-reading', () => {
    const ws = sink();
    ignore(stalled().pipeTo(ws));
    return [() => ignore(ws.abort('abort'))];
  }],
  ['pipeTo-signal-while-reading', () => {
    const ac = new AbortController();
    ignore(stalled().pipeTo(sink(), { signal: ac.signal }));
    return [() => ac.abort('signal')];
  }],
  ['pipeTo-signal-mid-pipe', () => {
    const ac = new AbortController();
    ignore(source(10).pipeTo(sink({ write: () => tick() }), { signal: ac.signal }));
    return [() => ac.abort('signal')];
  }],
  ['pipeTo-source-errors', () => {
    let rc;
    ignore(source(0, { start(c) { rc = c; }, pull() {} }).pipeTo(sink({ abort: () => tick() })));
    return [() => rc.error(new Error('source'))];
  }],
  ['pipeTo-source-closes', () => {
    let rc;
    ignore(source(0, { start(c) { rc = c; }, pull() {} }).pipeTo(sink({ close: () => tick() })));
    return [() => { rc.enqueue('a'); rc.close(); }];
  }],
  ['pipeTo-prevent-all', () => {
    const ac = new AbortController();
    ignore(stalled().pipeTo(sink(),
        { preventClose: true, preventAbort: true, preventCancel: true, signal: ac.signal }));
    return [() => ac.abort('signal')];
  }],
  ['pipeThrough', () => {
    const ts = keep(new TransformStream({
      transform(chunk, c) { work(); c.enqueue(`${chunk}!`); },
      flush(c) { work(); c.enqueue('end'); },
    }));
    ignore(readAll(source(3).pipeThrough(ts)));
  }],
  ['pipeThrough-cancel-readable', () => {
    const out = source(10).pipeThrough(keep(new TransformStream({ cancel() { work(); } })));
    const reader = keep(out.getReader());
    ignore(reader.read());
    return [() => ignore(reader.cancel('cancel'))];
  }],
  ['transform-writable-aborted', () => {
    const ts = keep(new TransformStream({ transform(chunk, c) { c.enqueue(chunk); } }));
    const writer = keep(ts.writable.getWriter());
    ignore(writer.write('a'));
    ignore(readAll(ts.readable));
    return [() => ignore(writer.abort('abort'))];
  }],
  ['tee', () => {
    const [a, b] = source(4).tee();
    keep(a, b);
    ignore(readAll(a));
    return [() => ignore(b.cancel('cancel'))];
  }],
  ['tee-cancel-both-while-reading', () => {
    const [a, b] = stalled().tee();
    const ra = keep(a.getReader());
    const rb = keep(b.getReader());
    ignore(ra.read());
    ignore(rb.read());
    return [() => ignore(ra.cancel('a')), () => ignore(rb.cancel('b'))];
  }],
  ['reader-cancel-while-reading', () => {
    const reader = keep(stalled().getReader());
    ignore(reader.read());
    return [() => ignore(reader.cancel('cancel'))];
  }],
  ['reader-release-while-reading', () => {
    const reader = keep(stalled().getReader());
    ignore(reader.read());
    return [() => reader.releaseLock()];
  }],
  ['async-iterator-break', async () => {
    for await (const chunk of source(5)) { work(); break; }
  }],
  ['enqueue-outside-pull', () => {
    let rc;
    const reader = keep(source(0, { start(c) { rc = c; }, pull() {} }).getReader());
    ignore(reader.read());
    ignore(reader.read());
    return [() => { rc.enqueue('a'); rc.enqueue('b'); rc.close(); }];
  }],
  ['error-outside-pull', () => {
    let rc;
    const reader = keep(source(0, { start(c) { rc = c; }, pull() {} }).getReader());
    ignore(reader.read());
    return [() => rc.error(new Error('source'))];
  }],
  ['writer-abort', () => {
    const writer = keep(sink({ write: () => tick() }).getWriter());
    ignore(writer.write('a'));
    ignore(writer.write('b'));
    return [() => { ignore(writer.abort('abort')); ignore(writer.close()); }];
  }],
  ['writer-close', () => {
    const writer = keep(sink({ write: () => tick(), close: () => tick() }).getWriter());
    ignore(writer.write('a'));
    return [() => ignore(writer.close())];
  }],
  ['writer-release-while-writing', () => {
    const writer = keep(sink({ write: () => tick(4) }).getWriter());
    ignore(writer.write('a'));
    ignore(writer.write('b'));
    return [() => writer.releaseLock()];
  }],
  ['sink-controller-error-while-writing', () => {
    let wc;
    const writer = keep(sink({ start(c) { wc = c; }, write: () => tick(4) }).getWriter());
    ignore(writer.write('a'));
    ignore(writer.write('b'));
    return [() => wc.error(new Error('sink'))];
  }],
  ['bytes-byob', () => {
    let n = 0;
    const rs = keep(new ReadableStream({
      type: 'bytes',
      pull(c) {
        work();
        const req = c.byobRequest;
        if (n++ > 2) { c.close(); if (req) req.respond(0); return; }
        if (req) { req.view[0] = n; req.respond(1); } else { c.enqueue(new Uint8Array([n])); }
      },
      cancel() { work(); },
    }));
    const reader = keep(rs.getReader({ mode: 'byob' }));
    ignore(reader.read(new Uint8Array(4)).then(() => reader.read(new Uint8Array(4))));
    return [() => ignore(reader.cancel('cancel'))];
  }],
  ['bytes-enqueue-outside-pull', () => {
    let rc;
    const rs = keep(new ReadableStream({ type: 'bytes', start(c) { keep(c); rc = c; } }));
    const reader = keep(rs.getReader({ mode: 'byob' }));
    ignore(reader.read(new Uint8Array(4)));
    return [() => { rc.enqueue(new Uint8Array([1, 2])); rc.close(); }];
  }],
  ['then-getter', () => {
    let rc;
    const reader = keep(source(0, { start(c) { rc = c; }, pull() {} }).getReader());
    ignore(reader.read());
    return [() => {
      Object.defineProperty(Object.prototype, 'then', {
        configurable: true,
        get() { work(); return undefined; },
      });
      try {
        rc.enqueue('a');
      } finally {
        delete Object.prototype.then;
      }
    }];
  }],
];

function post() {
  delete Object.prototype.then;
  for (const x of live) {
    try {
      if (x instanceof ReadableStream) {
        if (!x.locked) ignore(x.getReader().read());
        ignore(x.cancel('post'));
      } else if (x instanceof WritableStream) {
        if (!x.locked) {
          const writer = x.getWriter();
          ignore(writer.write('post'));
          ignore(writer.close());
        }
        ignore(x.abort('post'));
      } else if (x instanceof ReadableStreamDefaultReader) {
        ignore(x.read());
        ignore(x.cancel('post'));
      } else if (x instanceof WritableStreamDefaultWriter) {
        ignore(x.write('post'));
        ignore(x.abort('post'));
      } else if (x instanceof ReadableStreamDefaultController) {
        try { x.enqueue('post'); } catch (e) { check(e); }
        try { x.close(); } catch (e) { check(e); }
        try { x.error(new Error('post')); } catch (e) { check(e); }
      } else if (x instanceof ReadableByteStreamController) {
        try { x.enqueue(new Uint8Array([1])); } catch (e) { check(e); }
        try { x.close(); } catch (e) { check(e); }
        try { x.error(new Error('post')); } catch (e) { check(e); }
      } else if (x instanceof WritableStreamDefaultController) {
        try { x.error(new Error('post')); } catch (e) { check(e); }
      } else if (x instanceof TransformStream) {
        ignore(x.readable.cancel('post'));
        ignore(x.writable.abort('post'));
      }
    } catch (e) { check(e); }
  }
}

globalThis.harness = {
  count: scenarios.length,
  name: (i) => scenarios[i][0],
  run: (i) => {
    const steps = scenarios[i][1]();
    pending = Array.isArray(steps) ? steps : [];
  },
  // Runs the scenario's next step. Returns false once there are none left.
  next: () => {
    const step = pending.shift();
    if (!step) return false;
    step();
    return true;
  },
  post,
  reset: () => { live.length = 0; pending = []; delete Object.prototype.then; },
  takeInternalErrors: () => internalErrors.splice(0).join('\n'),
  probe: () => {},
};

export default {
  fetch() { return new Response('OK'); },
};
)SCRIPT"_kj;

jsg::JsValue callHarness(jsg::Lock& js, kj::StringPtr method, auto... args) {
  auto harness = KJ_ASSERT_NONNULL(js.global().get(js, "harness"_kj).tryCast<jsg::JsObject>());
  auto fn = KJ_ASSERT_NONNULL(harness.get(js, method).tryCast<jsg::JsFunction>());
  return fn.call(js, harness, args...);
}

// Terminates execution if a run takes too long. A stream left looping forever by an unwind shows
// up this way: its continuations keep queuing each other, so the microtask queue never drains.
class Watchdog {
 public:
  Watchdog(v8::Isolate* isolate, kj::Duration timeout)
      : thread([this, isolate, timeout]() {
          done.when([](bool d) { return d; }, [](bool&) {}, timeout);
          if (!*done.lockShared()) {
            fired = true;
            isolate->TerminateExecution();
          }
        }) {}

  ~Watchdog() noexcept(false) {
    *done.lockExclusive() = true;
  }

  bool hasFired() const {
    return fired;
  }

 private:
  kj::MutexGuarded<bool> done{false};
  std::atomic<bool> fired{false};
  // Declared last so that it is joined before the members it uses are destroyed.
  kj::Thread thread;
};

constexpr kj::Duration RUN_TIMEOUT = 2 * kj::SECONDS;

struct RunResult {
  uint64_t calls = 0;
  kj::Vector<kj::String> failures;
};

// Runs scenario `index`, terminating execution at the `terminateAt`th call into JavaScript
// (never if 0), then exercises what is left behind.
RunResult runScenario(TestFixture& fixture, uint index, uint64_t terminateAt) {
  RunResult result;
  auto fail = [&](kj::StringPtr phase, auto&&... details) {
    result.failures.add(kj::str(phase, ": ", details...));
  };

  kj::Maybe<Watchdog> watchdog;
  try {
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      auto& js = env.js;
      auto* isolate = js.v8Isolate;
      // A previous run's watchdog may have fired after that run released the isolate.
      isolate->CancelTerminateExecution();
      auto& dog = watchdog.emplace(isolate, RUN_TIMEOUT);
      v8::TryCatch tryCatch(isolate);

      // Runs one phase, recording anything other than the termination we injected. Returns
      // whether execution was terminating when the phase ended, either by unwinding it or by
      // returning with the termination still pending.
      auto phase = [&](kj::StringPtr name, bool terminationExpected, auto&& body) {
        bool terminated = false;
        try {
          body();
          terminated = isolate->IsExecutionTerminating();
        } catch (jsg::JsExceptionThrown&) {
          terminated = isolate->IsExecutionTerminating();
          if (dog.hasFired()) {
            fail(name, "did not finish within ", RUN_TIMEOUT);
          } else if (!terminationExpected || !terminated) {
            fail(name, "unexpected JavaScript exception");
          }
        } catch (kj::Exception& e) {
          fail(name, e);
        }
        if (!dog.hasFired()) isolate->CancelTerminateExecution();
        tryCatch.Reset();
        return terminated;
      };

      // Phase 1: run the scenario with termination armed.
      isolate->AddBeforeCallEnteredCallback(&TerminationInjector::onBeforeCallEntered);
      TerminationInjector::arm(terminateAt);
      bool terminated = phase("run"_kj, true, [&]() {
        // V8 delivers a requested termination at JavaScript's next interrupt check, which a step
        // may return without reaching. Enter JavaScript once more after each step, so that the
        // termination isn't first delivered in a microtask, where V8 ends it itself.
        auto step = [&](kj::StringPtr method, auto... args) {
          auto result = callHarness(js, method, args...);
          callHarness(js, "probe"_kj);
          return result;
        };
        step("run"_kj, js.num(static_cast<double>(index)));
        js.runMicrotasks();
        while (step("next"_kj).isTruthy(js)) {
          js.runMicrotasks();
        }
      });
      result.calls = TerminationInjector::calls;
      // Once injected, the termination must reach the end of the phase. If something caught it
      // and carried on, the scenario ran on as if execution could continue. Inside a microtask,
      // V8 itself ends the termination, and debug builds of V8 check there that nothing
      // swallowed it first.
      if (terminateAt != 0 && result.calls >= terminateAt &&
          !TerminationInjector::injectedInMicrotask && !terminated && !dog.hasFired()) {
        fail("run"_kj, "termination was swallowed");
      }
      TerminationInjector::arm(0);
      isolate->RemoveBeforeCallEnteredCallback(&TerminationInjector::onBeforeCallEntered);

      // Phase 2: with execution allowed again, poke everything the scenario created and let
      // the remaining continuations run.
      phase("post"_kj, false, [&]() {
        callHarness(js, "post"_kj);
        js.runMicrotasks();
        callHarness(js, "reset"_kj);
        js.runMicrotasks();
      });

      // Phase 3: collect whatever is now unreachable, and run what that queues.
      phase("gc"_kj, false, [&]() {
        isolate->LowMemoryNotification();
        js.runMicrotasks();
      });

      phase("check"_kj, false, [&]() {
        auto internalErrors = kj::str(callHarness(js, "takeInternalErrors"_kj));
        if (internalErrors.size() > 0) {
          fail("check"_kj, "C++ exceptions surfaced as JavaScript errors:\n", internalErrors);
        }
      });
    });
  } catch (kj::Exception& e) {
    fail("teardown"_kj, e);
  }
  KJ_IF_SOME(dog, watchdog) {
    if (dog.hasFired() && result.failures.empty()) {
      fail("teardown"_kj, "did not finish within ", RUN_TIMEOUT);
    }
  }
  return result;
}

KJ_TEST("streams survive termination at every call into JavaScript") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  SimpleWorkerErrorReporter errorReporter;
  compileCompatibilityFlags("2026-10-01"_kj, kj::ArrayPtr<const kj::String>(), flags, errorReporter,
      false, CompatibilityDateValidation::FUTURE_FOR_TEST, nullptr);

  kj::Vector<kj::String> failures;
  uint scenarioCount = 0;
  // Set TIJ_FILTER to a scenario name to run only that scenario.
  kj::Maybe<kj::StringPtr> filterName;
  if (const char* filter = getenv("TIJ_FILTER")) {
    filterName = kj::StringPtr(filter);
  }
  {
    TestFixture fixture({.featureFlags = flags.asReader(), .mainModuleSource = SCENARIOS});
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      auto harness =
          KJ_ASSERT_NONNULL(env.js.global().get(env.js, "harness"_kj).tryCast<jsg::JsObject>());
      auto count = KJ_ASSERT_NONNULL(harness.get(env.js, "count"_kj).tryCast<jsg::JsNumber>());
      scenarioCount = KJ_ASSERT_NONNULL(count.value(env.js));
    });
  }

  for (uint index: kj::zeroTo(scenarioCount)) {
    TestFixture fixture({.featureFlags = flags.asReader(), .mainModuleSource = SCENARIOS});
    kj::String name;
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      name = kj::str(callHarness(env.js, "name"_kj, env.js.num(static_cast<double>(index))));
    });

    KJ_IF_SOME(filter, filterName) {
      if (name != filter) continue;
    }

    auto baseline = runScenario(fixture, index, 0);
    for (auto& f: baseline.failures) {
      failures.add(kj::str(name, " (no termination) ", f));
      KJ_LOG(WARNING, failures.back());
    }
    uint64_t points = kj::min(baseline.calls, MAX_INJECTION_POINTS);
    KJ_LOG(INFO, "scenario", name, baseline.calls, points);

    for (uint64_t at = 1; at <= points; ++at) {
      KJ_LOG(INFO, "terminating", name, at);
      auto run = runScenario(fixture, index, at);
      for (auto& f: run.failures) {
        failures.add(kj::str(name, " (terminated at call ", at, ") ", f));
        KJ_LOG(WARNING, failures.back());
      }
    }
  }

  if (!failures.empty()) {
    KJ_FAIL_EXPECT(failures.size(), kj::strArray(failures, "\n"));
  }
}

}  // namespace
}  // namespace workerd::api
