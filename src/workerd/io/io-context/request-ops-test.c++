// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests the io-context crate's operations behind ctx.waitUntil(), ctx.passThroughOnException()
// and ctx.abort(): each scenario runs once on the C++ ExecutionContext and once on
// RustExecutionContext, which test_bridge.rs implements with the crate, and the two must be
// observed alike, by JavaScript, by the client and in the logs.

#include <workerd/io/io-context/ffi.h>
#include <workerd/io/io-context/test_bridge.rs.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/compat/http.h>
#include <kj/test.h>

#include <bit>

namespace workerd::io_context::test {
namespace {

uint64_t idOf(const IoContext& context) {
  return std::bit_cast<uint64_t>(context.getId());
}

// Each request runs the scenario its path names, on `ctx`, or on `rustCtx` if `useRust` is set,
// and records what it observes in `events`.
constexpr kj::StringPtr SCENARIOS = R"SCRIPT(
  globalThis.events = "";
  function event(text) {
    globalThis.events += (events ? "," : "") + text;
  }

  function abortWith(abort) {
    event("before");
    try {
      abort();
      event("returned");
    } catch (e) {
      event(`caught ${e}`);
    } finally {
      event("finally");
    }
    event("after");
    return new Response("not aborted");
  }

  const scenarios = {
    waitUntilGate(ctx) {
      ctx.waitUntil(gate.wait().then(() => event("waited")));
      event("responding");
      return new Response("ok");
    },
    gateWithoutWaitUntil(ctx) {
      gate.wait().then(() => event("waited"));
      event("responding");
      return new Response("ok");
    },
    waitUntilRejected(ctx) {
      ctx.waitUntil(Promise.reject(new RangeError("rejected in waitUntil")));
      return new Response("ok");
    },
    waitUntilValues(ctx) {
      for (const value of [42, "text", null, undefined, {}, () => {}]) ctx.waitUntil(value);
      ctx.waitUntil({ then(resolve) { event("then called"); resolve(); } });
      ctx.waitUntil(new Proxy({}, {
        has(target, key) { event(`has ${String(key)}`); return false; },
      }));
      return new Response("ok");
    },
    waitUntilRejectingThenable(ctx) {
      ctx.waitUntil({ then(resolve, reject) { reject(new RangeError("rejected by thenable")); } });
      return new Response("ok");
    },
    waitUntilWithoutArgument(ctx) {
      try {
        ctx.waitUntil();
        event("returned");
      } catch (e) {
        event(`caught ${e}`);
      }
      return new Response("ok");
    },
    waitUntilThrowingHas(ctx) {
      try {
        ctx.waitUntil(new Proxy({}, { has() { throw new TypeError("from has"); } }));
        event("returned");
      } catch (e) {
        event(`caught ${e}`);
      }
      return new Response("ok");
    },
    passThrough(ctx) {
      ctx.passThroughOnException();
      throw new Error("handler failed");
    },
    noPassThrough(ctx) {
      throw new Error("handler failed");
    },
    abortWithoutReason(ctx) { return abortWith(() => ctx.abort()); },
    abortUndefined(ctx) { return abortWith(() => ctx.abort(undefined)); },
    abortNull(ctx) { return abortWith(() => ctx.abort(null)); },
    abortError(ctx) { return abortWith(() => ctx.abort(new RangeError("custom reason"))); },
    abortString(ctx) { return abortWith(() => ctx.abort("string reason")); },
    abortRetryable(ctx) {
      return abortWith(() => ctx.abort({ retryable: true, message: "retry me" }));
    },
    abortThrowingReason(ctx) {
      return abortWith(() => ctx.abort({ get overloaded() { throw new TypeError("from getter"); } }));
    },
    stash(ctx) {
      globalThis.stashedCtx = ctx;
      return new Response("ok");
    },
  };

  // The arguments callOutsideRequest() can pass, by name.
  const outsideArgs = {
    reason: "reason",
    throwingReason: {
      get overloaded() {
        event("getter ran");
        throw new TypeError("from getter");
      },
    },
  };

  globalThis.callOutsideRequest = (target, method, argName) => {
    try {
      target[method](outsideArgs[argName]);
      return "returned";
    } catch (e) {
      return `threw ${e}`;
    }
  };

  export default {
    fetch(request, env, ctx) {
      return scenarios[new URL(request.url).pathname.slice(1)](globalThis.useRust ? rustCtx : ctx);
    },
  };
)SCRIPT"_kj;

enum class Implementation { CPP, RUST };

// What a scenario's request reports to its RequestObserver and IoChannelFactory.
struct RequestRecord: public kj::Refcounted {
  kj::Vector<kj::String> failures;
  kj::Vector<kj::String> failureLocations;
  kj::Vector<kj::String> failedOpen;
  kj::Vector<kj::String> subrequests;
};

class RecordingRequestObserver final: public RequestObserver {
 public:
  explicit RecordingRequestObserver(kj::Own<RequestRecord> record): record(kj::mv(record)) {}

  void reportFailure(const kj::Exception& e, FailureSource) override {
    record->failures.add(kj::str(e.getType(), ": ", e.getDescription()));
    // Logs show where the exception was created, so that must match too.
    record->failureLocations.add(kj::str(e.getFile(), ":", e.getLine()));
  }
  void setFailedOpen(bool value) override {
    record->failedOpen.add(kj::str(value));
  }

 private:
  kj::Own<RequestRecord> record;
};

// Answers the subrequest a failed-open request makes to its origin with a disconnection.
class OriginlessIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
 public:
  OriginlessIoChannelFactory(TimerChannel& timer, kj::Own<RequestRecord> record)
      : DummyIoChannelFactory(timer),
        record(kj::mv(record)) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata) override {
    record->subrequests.add(kj::str(channel));
    return WorkerInterface::fromException(KJ_EXCEPTION(DISCONNECTED, "there is no origin"));
  }

 private:
  kj::Own<RequestRecord> record;
};

class StatusResponse final: public kj::HttpService::Response {
 public:
  kj::Own<kj::AsyncOutputStream> send(
      uint statusCode, kj::StringPtr, const kj::HttpHeaders&, kj::Maybe<uint64_t>) override {
    this->statusCode = statusCode;
    return kj::heap<kj::NullStream>();
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders&) override {
    KJ_FAIL_ASSERT("no WebSockets");
  }

  uint statusCode = 0;
};

// Records the uncaught exceptions the worker logs, which it logs only at INFO level, while it
// exists, and passes on other messages the log level allowed before.
class UncaughtExceptionLog final: public kj::ExceptionCallback {
 public:
  UncaughtExceptionLog(): previousLevel(currentLogLevel()) {
    kj::_::Debug::setLogLevel(kj::LogSeverity::INFO);
  }
  ~UncaughtExceptionLog() noexcept(false) {
    kj::_::Debug::setLogLevel(previousLevel);
  }

  void logMessage(kj::LogSeverity severity,
      const char* file,
      int line,
      int contextDepth,
      kj::String&& text) override {
    if (text.contains("uncaught exception")) {
      messages.add(kj::mv(text));
    } else if (severity >= previousLevel) {
      next.logMessage(severity, file, line, contextDepth, kj::mv(text));
    }
  }

  kj::Vector<kj::String> messages;

 private:
  kj::LogSeverity previousLevel;

  // KJ has no getter for the log level.
  static kj::LogSeverity currentLogLevel() {
    for (auto severity: {kj::LogSeverity::INFO, kj::LogSeverity::WARNING, kj::LogSeverity::ERROR,
           kj::LogSeverity::FATAL}) {
      if (kj::_::Debug::shouldLog(severity)) return severity;
    }
    return kj::LogSeverity::DBG;
  }
};

// Everything a scenario lets the test observe.
struct Outcome {
  uint statusCode;
  kj::String events;
  kj::String failures;
  kj::String failureLocations;
  kj::String failedOpen;
  kj::String subrequests;
  kj::String uncaughtExceptions;

  kj::String toString() const {
    return kj::str("status: ", statusCode, "\nevents: ", events, "\nfailures: ", failures,
        "\nfailure locations: ", failureLocations, "\nfailedOpen: ", failedOpen,
        "\nsubrequests: ", subrequests, "\nuncaught exceptions: ", uncaughtExceptions);
  }
};

// Runs `body` in the worker's context, outside any request.
template <typename Func>
void inWorkerContext(TestFixture& fixture, Func&& body) {
  fixture.enterWorkerLock([&](Worker::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](jsg::Lock& js) { body(js); });
  });
}

jsg::JsValue fromRust(jsg::Lock& js, ::workerd::rust::jsg::MaybeLocal value) {
  auto handle = ::workerd::rust::jsg::maybe_local_from_ffi<v8::Value>(kj::mv(value));
  return jsg::JsValue(handle.ToLocalChecked());
}

kj::String readEvents(TestFixture& fixture) {
  kj::String events;
  inWorkerContext(
      fixture, [&](jsg::Lock& js) { events = js.global().get(js, "events"_kj).toString(js); });
  return events;
}

// A worker running SCENARIOS, with `rustCtx` and `gate` set and `useRust` set for RUST.
struct ScenarioWorker {
  kj::Own<RequestRecord> record = kj::refcounted<RequestRecord>();
  capnp::MallocMessageBuilder flagsMessage;
  TestFixture fixture;

  ScenarioWorker(Implementation implementation, bool unwrapCustomThenables)
      : fixture(params(unwrapCustomThenables)) {
    inWorkerContext(fixture, [&](jsg::Lock& js) {
      auto isolate = js.v8Isolate;
      auto global = js.global();
      global.set(js, "rustCtx", fromRust(js, new_rust_execution_context(isolate)));
      global.set(js, "gate", fromRust(js, new_gate(isolate)));
      global.set(js, "useRust", js.boolean(implementation == Implementation::RUST));
    });
  }

  TestFixture::SetupParams params(bool unwrapCustomThenables) {
    auto flags = flagsMessage.initRoot<CompatibilityFlags>();
    flags.setUnwrapCustomThenables(unwrapCustomThenables);
    return {
      .featureFlags = flags.asReader(),
      .mainModuleSource = SCENARIOS,
      .useRealTimers = false,
      .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
          [this](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
      return kj::rc<OriginlessIoChannelFactory>(timer, kj::addRef(*record));
    }),
      .requestObserverFactory =
          kj::Function<kj::Own<RequestObserver>()>([this]() -> kj::Own<RequestObserver> {
      return kj::refcounted<RecordingRequestObserver>(kj::addRef(*record));
    }),
    };
  }

  // Sends a request for `scenario` and waits for its response.
  uint request(kj::StringPtr scenario) {
    auto entrypoint = fixture.makeWorkerEntrypoint();
    kj::HttpHeaderTable headerTable;
    kj::HttpHeaders headers(headerTable);
    kj::NullStream requestBody;
    StatusResponse response;
    entrypoint
        ->request(kj::HttpMethod::GET, kj::str("https://example.com/", scenario), headers,
            requestBody, response)
        .wait(fixture.getWaitScope());
    return response.statusCode;
  }

  Outcome outcome(uint statusCode, UncaughtExceptionLog& log) {
    return {
      .statusCode = statusCode,
      .events = readEvents(fixture),
      .failures = kj::strArray(record->failures, " | "),
      .failureLocations = kj::strArray(record->failureLocations, " | "),
      .failedOpen = kj::strArray(record->failedOpen, ","),
      .subrequests = kj::strArray(record->subrequests, ","),
      .uncaughtExceptions = kj::strArray(log.messages, " | "),
    };
  }
};

// Runs `scenario` on the given implementation and returns what was observed once the request
// and everything it waits for are done.
Outcome run(kj::StringPtr scenario, Implementation implementation, bool unwrapCustomThenables) {
  UncaughtExceptionLog log;
  ScenarioWorker worker(implementation, unwrapCustomThenables);
  auto statusCode = worker.request(scenario);
  worker.fixture.pollEventLoop();
  return worker.outcome(statusCode, log);
}

// Runs `scenario` on both implementations, checks that they are observed alike, and returns
// the outcome.
Outcome runOnBoth(kj::StringPtr scenario, bool unwrapCustomThenables = false) {
  auto cpp = run(scenario, Implementation::CPP, unwrapCustomThenables);
  auto rust = run(scenario, Implementation::RUST, unwrapCustomThenables);
  KJ_EXPECT(cpp.toString() == rust.toString(), scenario, cpp.toString(), rust.toString());
  return cpp;
}

KJ_TEST("waitUntil() keeps the request running after the response, as in C++") {
  for (auto implementation: {Implementation::CPP, Implementation::RUST}) {
    ScenarioWorker worker(implementation, false);
    KJ_EXPECT(worker.request("waitUntilGate") == 200);
    worker.fixture.pollEventLoop();
    KJ_EXPECT(readEvents(worker.fixture) == "responding");

    // The request is still there to run the reaction once the gate opens.
    KJ_EXPECT(open_gate());
    worker.fixture.pollEventLoop();
    KJ_EXPECT(readEvents(worker.fixture) == "responding,waited");
  }
}

KJ_TEST("without waitUntil(), the request does not wait for the gate") {
  // The control for the test above: the reaction runs only because of waitUntil().
  ScenarioWorker worker(Implementation::CPP, false);
  KJ_EXPECT(worker.request("gateWithoutWaitUntil") == 200);
  worker.fixture.pollEventLoop();
  // The request is done, and the I/O it awaited was canceled with it.
  KJ_EXPECT(!open_gate());
  worker.fixture.pollEventLoop();
  KJ_EXPECT(readEvents(worker.fixture) == "responding");
}

KJ_TEST("a rejected waitUntil() promise is logged as an uncaught exception, as in C++") {
  auto outcome = runOnBoth("waitUntilRejected");
  KJ_EXPECT(outcome.statusCode == 200);
  KJ_EXPECT(outcome.failures == "");
  KJ_EXPECT(outcome.uncaughtExceptions.contains("RangeError: rejected in waitUntil"),
      outcome.uncaughtExceptions);
}

KJ_TEST("waitUntil() converts other values as C++ does, following thenables only with the flag") {
  auto withoutFlag = runOnBoth("waitUntilValues", false);
  KJ_EXPECT(withoutFlag.statusCode == 200);
  KJ_EXPECT(withoutFlag.events == "", withoutFlag.events);
  KJ_EXPECT(withoutFlag.uncaughtExceptions == "", withoutFlag.uncaughtExceptions);

  auto withFlag = runOnBoth("waitUntilValues", true);
  KJ_EXPECT(withFlag.statusCode == 200);
  KJ_EXPECT(withFlag.events == "has then,then called", withFlag.events);
  KJ_EXPECT(withFlag.uncaughtExceptions == "", withFlag.uncaughtExceptions);
}

KJ_TEST("a followed thenable that rejects is logged as an uncaught exception, as in C++") {
  auto withFlag = runOnBoth("waitUntilRejectingThenable", true);
  KJ_EXPECT(withFlag.statusCode == 200);
  KJ_EXPECT(withFlag.uncaughtExceptions.contains("RangeError: rejected by thenable"),
      withFlag.uncaughtExceptions);

  auto withoutFlag = runOnBoth("waitUntilRejectingThenable", false);
  KJ_EXPECT(withoutFlag.uncaughtExceptions == "", withoutFlag.uncaughtExceptions);
}

KJ_TEST("waitUntil() requires an argument, as in C++") {
  auto outcome = runOnBoth("waitUntilWithoutArgument");
  KJ_EXPECT(outcome.statusCode == 200);
  KJ_EXPECT(outcome.events ==
          "caught TypeError: Failed to execute 'waitUntil' on 'ExecutionContext': parameter 1 is "
          "not of type 'Promise'.",
      outcome.events);
}

KJ_TEST("what converting a waitUntil() value throws reaches JavaScript, as in C++") {
  auto outcome = runOnBoth("waitUntilThrowingHas", true);
  KJ_EXPECT(outcome.statusCode == 200);
  KJ_EXPECT(outcome.events == "caught TypeError: from has", outcome.events);
}

KJ_TEST("passThroughOnException() has a failing request fail open, as in C++") {
  auto passThrough = runOnBoth("passThrough");
  KJ_EXPECT(passThrough.subrequests == kj::str(IoContext::NEXT_CLIENT_CHANNEL));
  // The origin fails, so the client gets an error after all.
  KJ_EXPECT(passThrough.failedOpen == "true,false", passThrough.failedOpen);
  KJ_EXPECT(passThrough.statusCode == 500);

  auto control = runOnBoth("noPassThrough");
  KJ_EXPECT(control.subrequests == "");
  KJ_EXPECT(control.failedOpen == "");
  KJ_EXPECT(control.statusCode == 500);
}

KJ_TEST("abort() without a reason aborts and terminates as C++ does") {
  for (auto scenario: {"abortWithoutReason"_kj, "abortUndefined"_kj}) {
    auto outcome = runOnBoth(scenario);
    // No more JavaScript runs, and nothing is caught.
    KJ_EXPECT(outcome.events == "before", scenario, outcome.events);
    KJ_EXPECT(outcome.failures ==
            "failed: jsg.Error: Worker execution was aborted due to call to ctx.abort().",
        scenario, outcome.failures);
    KJ_EXPECT(outcome.statusCode == 500, scenario);
    KJ_EXPECT(outcome.failureLocations.contains("io-context.c++"), outcome.failureLocations);
  }
}

KJ_TEST("abort() with a reason aborts with it, as C++ encodes it, and terminates") {
  auto error = runOnBoth("abortError");
  KJ_EXPECT(error.events == "before", error.events);
  KJ_EXPECT(error.failures == "failed: jsg.RangeError: custom reason", error.failures);
  KJ_EXPECT(error.statusCode == 500);

  auto string = runOnBoth("abortString");
  KJ_EXPECT(string.events == "before", string.events);
  KJ_EXPECT(string.failures.contains("string reason"), string.failures);
  KJ_EXPECT(string.statusCode == 500);

  // null is a reason, not the absence of one.
  auto null = runOnBoth("abortNull");
  KJ_EXPECT(null.events == "before", null.events);
  KJ_EXPECT(null.failures.contains("null"), null.failures);
  KJ_EXPECT(null.statusCode == 500);

  auto retryable = runOnBoth("abortRetryable");
  KJ_EXPECT(retryable.events == "before", retryable.events);
  KJ_EXPECT(retryable.failures.startsWith("disconnected: "), retryable.failures);
}

KJ_TEST("abort() passes on what encoding its reason throws, aborting nothing, as C++ does") {
  auto outcome = runOnBoth("abortThrowingReason");
  KJ_EXPECT(outcome.events == "before,caught TypeError: from getter,finally,after", outcome.events);
  KJ_EXPECT(outcome.failures == "", outcome.failures);
  KJ_EXPECT(outcome.statusCode == 200);
}

// What calling `method` of the object global `target` with the argument `argName` names in
// SCENARIOS does outside any request.
kj::String callOutsideRequest(
    TestFixture& fixture, kj::StringPtr target, kj::StringPtr method, kj::StringPtr argName) {
  kj::String result;
  inWorkerContext(fixture, [&](jsg::Lock& js) {
    auto global = js.global();
    v8::Local<v8::Value> call = global.get(js, "callOutsideRequest"_kj);
    v8::Local<v8::Value> args[] = {global.get(js, target), js.str(method), js.str(argName)};
    auto context = js.v8Context();
    auto value =
        jsg::check(call.As<v8::Function>()->Call(context, context->Global(), kj::size(args), args));
    result = jsg::JsValue(value).toString(js);
  });
  return result;
}

KJ_TEST("outside a request, the operations fail as C++'s do") {
  ScenarioWorker worker(Implementation::CPP, false);
  KJ_EXPECT(worker.request("stash") == 200);
  for (auto method: {"waitUntil"_kj, "passThroughOnException"_kj, "abort"_kj}) {
    auto cpp = callOutsideRequest(worker.fixture, "stashedCtx", method, "reason");
    auto rust = callOutsideRequest(worker.fixture, "rustCtx", method, "reason");
    KJ_EXPECT(cpp == rust, method, cpp, rust);
    KJ_EXPECT(cpp.startsWith("threw Error: Disallowed operation called within global scope."),
        method, cpp);
  }

  // abort() looks for the request before it encodes its reason, so the getter doesn't run.
  auto cpp = callOutsideRequest(worker.fixture, "stashedCtx", "abort", "throwingReason");
  auto rust = callOutsideRequest(worker.fixture, "rustCtx", "abort", "throwingReason");
  KJ_EXPECT(cpp == rust, cpp, rust);
  KJ_EXPECT(cpp.startsWith("threw Error: Disallowed operation called within global scope."), cpp);
  KJ_EXPECT(!readEvents(worker.fixture).contains("getter ran"), readEvents(worker.fixture));
}

KJ_TEST("the operations reject an IoContext id that is not the current one") {
  TestFixture fixture;
  auto first = fixture.newIncomingRequest();
  uint64_t firstId = 0;
  fixture.enterContext(
      *first, [&](const TestFixture::Environment& env) { firstId = idOf(env.context); });

  auto second = fixture.newIncomingRequest();
  fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
    auto promise = jsg::check(v8::Promise::Resolver::New(env.js.v8Context()))->GetPromise();
    KJ_EXPECT_THROW_MESSAGE("not the one of the current request",
        io_context_add_wait_until(firstId, ::workerd::rust::jsg::to_ffi(kj::mv(promise))));
    KJ_EXPECT_THROW_MESSAGE(
        "not the one of the current request", io_context_set_fail_open(firstId));
    KJ_EXPECT_THROW_MESSAGE("not the one of the current request",
        io_context_abort(firstId, ::workerd::rust::jsg::MaybeLocal{0}));
    KJ_EXPECT(!env.context.isFailOpen());
    KJ_EXPECT(env.context.getAbortReason() == kj::none);
  });

  inWorkerContext(fixture, [&](jsg::Lock& js) {
    auto promise = jsg::check(v8::Promise::Resolver::New(js.v8Context()))->GetPromise();
    KJ_EXPECT_THROW_MESSAGE("there is no current request",
        io_context_add_wait_until(firstId, ::workerd::rust::jsg::to_ffi(kj::mv(promise))));
    KJ_EXPECT_THROW_MESSAGE("there is no current request", io_context_set_fail_open(firstId));
    KJ_EXPECT_THROW_MESSAGE("there is no current request",
        io_context_abort(firstId, ::workerd::rust::jsg::MaybeLocal{0}));
  });

  fixture.enterContext(*first, [&](const TestFixture::Environment& env) {
    KJ_EXPECT(!env.context.isFailOpen());
    KJ_EXPECT(env.context.getAbortReason() == kj::none);
  });

  fixture.drainAndDestroy(kj::mv(second));
  fixture.drainAndDestroy(kj::mv(first));
}

KJ_TEST("add_wait_until() and abort() with a reason do nothing while terminating") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    request_ops_while_terminating(reinterpret_cast<uintptr_t>(env.isolate));
    KJ_EXPECT(env.isolate->IsExecutionTerminating());
    env.isolate->CancelTerminateExecution();
    KJ_EXPECT(env.context.getAbortReason() == kj::none);
  });
}

KJ_TEST("io_context_set_fail_open() sets the current IoContext to fail open") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(!env.context.isFailOpen());
    io_context_set_fail_open(idOf(env.context));
    KJ_EXPECT(env.context.isFailOpen());
  });
}

}  // namespace
}  // namespace workerd::io_context::test
