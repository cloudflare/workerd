// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/io-context.h>
#include <workerd/io/tracer.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/uncaught-exception-source.h>

#include <kj/test.h>

#include <atomic>

namespace workerd {
namespace {

struct TracedException {
  kj::String spanContext;
  kj::Date timestamp;
  kj::String name;
  kj::String message;
  kj::Maybe<kj::String> stack;

  bool operator==(const TracedException& other) const = default;

  kj::String toString() const {
    return kj::str("name: ", name, "\nmessage: ", message, "\nstack: ", stack,
        "\nspanContext: ", spanContext, "\ntimestamp: ", timestamp);
  }
};

// Records the exceptions added to the trace.
class ExceptionTracer final: public BaseTracer {
 public:
  kj::Vector<TracedException> exceptions;

  void addException(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack) override {
    exceptions.add(TracedException{
      .spanContext = kj::str(context),
      .timestamp = timestamp,
      .name = kj::mv(name),
      .message = kj::mv(message),
      .stack = kj::mv(stack),
    });
  }

  void setEventInfo(IoContext::IncomingRequest&, tracing::EventInfo&&) override {}
  void setReturn(kj::Maybe<kj::Date>, kj::Maybe<tracing::FetchResponseInfo>) override {}
  void addLog(const tracing::InvocationSpanContext&,
      kj::Date,
      LogLevel,
      kj::String,
      tracing::LogErrorInfo) override {}
  void addSpanOpen(tracing::SpanId, tracing::SpanId, kj::ConstString, kj::Date) override {}
  void addSpanClose(tracing::SpanEndData&&, kj::Maybe<kj::Date>) override {}
  void addSpanUpdate(tracing::SpanId, tracing::SpanUpdate&&) override {}
  void addSpanAttribute(
      const tracing::InvocationSpanContext&, kj::ConstString, tracing::Attribute::Value) override {}
  void addSpanException(tracing::SpanId,
      kj::Date,
      kj::Maybe<tracing::Exception::Code>,
      kj::String,
      kj::String,
      kj::Maybe<kj::String>) override {}
  void addDiagnosticChannelEvent(
      const tracing::InvocationSpanContext&, kj::Date, kj::String, kj::Array<kj::byte>) override {}
  void setOutcome(EventOutcome, kj::Duration, kj::Duration) override {}
  void recordTimestamp(kj::Date) override {}
  void setJsRpcInfo(
      const tracing::InvocationSpanContext&, kj::Date, const kj::ConstString&) override {}
  void markUnused() override {}
};

// A clock that stands still until it is synced, as the clock of a request does between visits to
// the isolate.
struct SyncedTimerChannel final: public TimerChannel {
  explicit SyncedTimerChannel(kj::Date time): actual(time), synced(time) {}

  void syncTime() override {
    synced = actual;
  }
  kj::Date now(kj::Maybe<kj::Date>) override {
    return synced;
  }
  kj::Promise<void> atTime(kj::Date) override {
    return kj::NEVER_DONE;
  }
  kj::Promise<void> afterLimitTimeout(kj::Duration) override {
    return kj::NEVER_DONE;
  }

  kj::Date actual;
  kj::Date synced;
};

constexpr kj::Date START_TIME = kj::UNIX_EPOCH + 1000 * kj::SECONDS;

// Counts the isolate locks taken.
class LockCounter final: public IsolateObserver {
 public:
  kj::Maybe<kj::Own<LockTiming>> tryCreateLockTiming(
      kj::OneOf<SpanParent, kj::Maybe<RequestObserver&>>) const override {
    ++count;
    return kj::none;
  }

  mutable std::atomic<uint> count = 0;
};

constexpr kj::StringPtr INTERNAL_ERROR_PREFIX = "internal error; reference = "_kj;

// A worker with a traced request in progress.
struct TracedWorker {
  explicit TracedWorker(TestFixture::SetupParams params = {})
      : fixture(withTimer(kj::mv(params), timer)),
        context(fixture.newIoContext()),
        tracer(kj::rc<ExceptionTracer>()),
        request(newRequest(tracer)) {}
  ~TracedWorker() noexcept(false) {
    fixture.drainAndDestroy(kj::mv(request));
  }

  static TestFixture::SetupParams withTimer(
      TestFixture::SetupParams params, SyncedTimerChannel& timer) {
    params.ioChannelFactory = [&timer](TimerChannel&) -> kj::Rc<IoChannelFactory> {
      return kj::rc<TestFixture::DummyIoChannelFactory>(timer);
    };
    return params;
  }

  kj::Own<IoContext::IncomingRequest> newRequest(kj::Rc<ExceptionTracer>& tracer) {
    auto request =
        fixture.newUndeliveredIncomingRequest(*context, kj::Rc<BaseTracer>(tracer.addRef()));
    request->delivered();
    return request;
  }

  // Runs `code`, which has to throw, and returns what it threw as a kj::Exception.
  kj::Exception tunnelThrown(kj::StringPtr code) {
    kj::Maybe<kj::Exception> result;
    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      env.js.tryCatch([&]() {
        // In a function, so that one script's declarations don't clash with another's.
        auto source = jsg::v8Str(env.isolate, kj::str("(() => {\n", code, "\n})();"));
        auto script = jsg::check(v8::Script::Compile(env.js.v8Context(), source));
        jsg::check(script->Run(env.js.v8Context()));
        KJ_FAIL_ASSERT("the code did not throw", code);
      }, [&](jsg::Value exception) { result = env.js.exceptionToKj(kj::mv(exception)); });
    });
    return KJ_ASSERT_NONNULL(kj::mv(result));
  }

  // Adds `exception` to the trace of `request` the way that needs the isolate lock: by converting
  // it to JavaScript and reading the properties of the resulting error.
  void traceUnderLock(const kj::Exception& exception) {
    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      auto error = env.js.exceptionToJsValue(exception.clone(), {.trusted = true});
      env.lock.logUncaughtException(UncaughtExceptionSource::REQUEST_HANDLER,
          error.getHandle(env.js), jsg::JsMessage(), *request);
    });
  }

  // Checks that logUncaughtExceptionAsync() adds the same thing to the trace as traceUnderLock(),
  // without taking the isolate lock, and returns it.
  TracedException expectSameTrace(kj::Exception exception, LockCounter& locks) {
    // Time has passed since the isolate was last entered.
    timer.actual += 1 * kj::SECONDS;
    traceUnderLock(exception);
    KJ_ASSERT(tracer->exceptions.size() == 1);
    auto expected = kj::mv(tracer->exceptions[0]);
    tracer->exceptions.clear();
    KJ_EXPECT(expected.timestamp == timer.actual);

    timer.actual += 1 * kj::SECONDS;
    expected.timestamp = timer.actual;
    auto locksBefore = locks.count.load();
    context->logUncaughtExceptionAsync(
        UncaughtExceptionSource::REQUEST_HANDLER, kj::mv(exception), *request);
    KJ_EXPECT(locks.count == locksBefore);
    KJ_ASSERT(tracer->exceptions.size() == 1);
    auto actual = kj::mv(tracer->exceptions[0]);
    tracer->exceptions.clear();

    // Each conversion of an internal exception refers to it by a new id.
    if (actual.message.startsWith(INTERNAL_ERROR_PREFIX) &&
        expected.message.startsWith(INTERNAL_ERROR_PREFIX) &&
        actual.message.size() == expected.message.size()) {
      expected.message = kj::str(actual.message);
    }
    KJ_EXPECT(actual == expected, actual, expected);
    return actual;
  }

  SyncedTimerChannel timer{START_TIME};
  TestFixture fixture;
  kj::Own<IoContext> context;
  kj::Rc<ExceptionTracer> tracer;
  kj::Own<IoContext::IncomingRequest> request;
};

kj::Exception failure(kj::StringPtr description) {
  return kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__, kj::str(description));
}

// Checks every kind of exception with and without `enhanced_error_serialization`, which decides
// how the JavaScript error that an exception carries was serialized.
void expectSameTraces(bool enhancedErrorSerialization) {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setEnhancedErrorSerialization(enhancedErrorSerialization);
  auto locks = kj::atomicRefcounted<LockCounter>();
  TracedWorker worker({
    .featureFlags = flags.asReader(),
    .isolateObserver = kj::atomicAddRef(*locks),
  });

  auto expectThrown = [&](kj::StringPtr code) {
    KJ_CONTEXT(code);
    return worker.expectSameTrace(worker.tunnelThrown(code), *locks);
  };
  auto expectTunneled = [&](kj::StringPtr description) {
    KJ_CONTEXT(description);
    return worker.expectSameTrace(failure(description), *locks);
  };
  // The exception is logged each time it is traced.
  auto expectInternal = [&](kj::Exception exception) {
    KJ_CONTEXT(exception.getDescription());
    KJ_EXPECT_LOG(ERROR, "sentryErrorContext = jsgInternalError; wdErrId = ");
    KJ_EXPECT_LOG(ERROR, "sentryErrorContext = jsgInternalError; wdErrId = ");
    auto traced = worker.expectSameTrace(kj::mv(exception), *locks);
    KJ_EXPECT(traced.message.startsWith(INTERNAL_ERROR_PREFIX), traced.message);
    KJ_EXPECT(traced.stack == kj::none);
    return traced;
  };

  // Taking the lock is counted, so the count shows that tracing an exception does not take it.
  auto locksBefore = locks->count.load();
  worker.fixture.enterContext(*worker.request, [](const TestFixture::Environment&) {});
  KJ_EXPECT(locks->count == locksBefore + 1);

  // Errors thrown by JavaScript.
  {
    auto traced = expectThrown("function thrower() { throw new Error('boom'); }\nthrower();");
    KJ_EXPECT(traced.name == "Error");
    KJ_EXPECT(traced.message == "boom");
    KJ_EXPECT(KJ_ASSERT_NONNULL(traced.stack).startsWith("    at thrower ("), traced.stack);
  }
  {
    auto traced = expectThrown("function thrower() { throw new TypeError('wrong'); }\nthrower();");
    KJ_EXPECT(traced.name == "TypeError");
    KJ_EXPECT(KJ_ASSERT_NONNULL(traced.stack).startsWith("    at thrower ("), traced.stack);
  }
  {
    auto traced = expectThrown("class CustomError extends Error {\n"
                               "  constructor(message) {\n"
                               "    super(message);\n"
                               "    this.name = 'CustomError';\n"
                               "  }\n"
                               "}\n"
                               "function thrower() { throw new CustomError('boom'); }\n"
                               "thrower();");
    KJ_EXPECT(traced.name == (enhancedErrorSerialization ? "CustomError"_kj : "Error"_kj));
    KJ_EXPECT(traced.message == "boom");
    KJ_EXPECT(traced.stack != kj::none);
  }
  expectThrown("class CustomError extends Error {}\n"
               "CustomError.prototype.name = 'CustomError';\n"
               "throw new CustomError('boom');");
  {
    auto traced = expectThrown("function thrower() { throw new Error('two\\nlines'); }\n"
                               "thrower();");
    KJ_EXPECT(traced.message == "two\nlines");
    KJ_EXPECT(KJ_ASSERT_NONNULL(traced.stack).startsWith("    at thrower ("), traced.stack);
  }
  expectThrown("throw new Error();");
  expectThrown("throw new Error('');");
  expectThrown("throw new RangeError('with a cause', { cause: new Error('the cause') });");
  expectThrown("throw new AggregateError([new Error('one')], 'several');");
  expectThrown("const e = new Error('renamed'); e.name = 'SyntaxError'; throw e;");
  expectThrown("const e = new Error('changed'); e.message = 'afterwards'; throw e;");
  expectThrown("const e = new Error('own stack'); e.stack = 'not a stack trace'; throw e;");
  expectThrown("const e = new Error('no stack'); e.stack = undefined; throw e;");
  expectThrown("const e = new Error('extra'); e.code = 42; throw e;");
  {
    // A DOMException is rebuilt from its name and message, which leaves no stack to trace.
    auto traced = expectThrown("function thrower() {\n"
                               "  throw new DOMException('aborted', 'AbortError');\n"
                               "}\n"
                               "thrower();");
    KJ_EXPECT(traced.name == "AbortError");
    KJ_EXPECT(traced.message == "aborted");
    KJ_EXPECT(traced.stack == kj::none);
  }
  expectThrown("throw new DOMException('no name');");
  expectThrown("throw new DOMException();");

  // Other things thrown by JavaScript.
  {
    auto traced = expectThrown("throw 'just a string';");
    KJ_EXPECT(traced.name == "Error");
    KJ_EXPECT(traced.message == "just a string");
    KJ_EXPECT(traced.stack == kj::none);
  }
  expectThrown("throw 42;");
  expectThrown("throw null;");
  expectThrown("throw undefined;");
  {
    auto traced = expectThrown("throw { name: 'Custom', message: 'from an object' };");
    KJ_EXPECT(traced.name == "Custom");
    KJ_EXPECT(traced.message == "from an object");
    KJ_EXPECT(traced.stack == kj::none);
  }
  expectThrown("throw { message: 'no name' };");
  expectThrown("throw { message: 'with a stack', stack: 'Error: with a stack\\n    at nowhere' };");
  {
    // Without a message, how the object is stringified stands in for one.
    auto traced = expectThrown("throw { name: 'Custom', code: 42 };");
    KJ_EXPECT(traced.name == "Custom");
    KJ_EXPECT(traced.message == "#<Object>");
  }
  expectThrown("throw { code: 42 };");
  expectThrown("throw [1, 2];");
  expectThrown("throw new Map();");
  expectThrown("throw function thrown() {};");
  expectThrown("throw new (class Thrown { constructor() { this.message = 'an instance'; } })();");
  // An error that cannot be serialized is rebuilt from its description, without its stack.
  expectThrown("const e = new Error('not serializable'); e.method = () => {}; throw e;");

  // Errors thrown by C++, which have only a description.
  {
    auto traced = expectTunneled("jsg.TypeError: thrown by C++");
    KJ_EXPECT(traced.name == "TypeError");
    KJ_EXPECT(traced.message == "thrown by C++");
    KJ_EXPECT(traced.stack == kj::none);
  }
  expectTunneled("jsg.Error: plain");
  expectTunneled("jsg.Error");
  expectTunneled("jsg.Error <unseen message>");
  expectTunneled("jsg.ErrorLike: message");
  expectTunneled("jsg.RangeError: range");
  expectTunneled("jsg.SyntaxError: syntax");
  expectTunneled("jsg.ReferenceError: reference");
  expectTunneled("jsg.CompileError: compile");
  expectTunneled("jsg.LinkError: link");
  expectTunneled("jsg.RuntimeError: runtime");
  expectTunneled("jsg.SuspendError: suspend");
  expectTunneled("jsg.AggregateError: aggregate");
  expectTunneled("jsg.SuppressedError: suppressed");
  expectTunneled("jsg.URIError: uri");
  expectTunneled("jsg.EvalError: eval");
  {
    auto traced = expectTunneled("jsg.DOMException(OperationError): thrown by C++");
    KJ_EXPECT(traced.name == "OperationError");
    KJ_EXPECT(traced.message == "thrown by C++");
    KJ_EXPECT(traced.stack == kj::none);
  }
  expectTunneled("jsg.DOMException(Some error): with a space");
  expectTunneled("jsg.DOMException(OperationError) <unseen message>");
  expectTunneled("jsg.DOMException(): no name");
  expectTunneled("jsg.DOMException(TypeError: unclosed");
  expectTunneled("remote.jsg.TypeError: from afar");
  expectTunneled("remote exception: remote.jsg.Error: over RPC");
  expectTunneled("broken.outputGateBroken; jsg.Error: gate broke");
  expectTunneled("remote.broken.inputGateBroken; jsg.DOMException(AbortError): aborted");
  expectTunneled("expected s.startsWith(\";\"); jsg.TypeError: required");
  worker.expectSameTrace(KJ_EXCEPTION(DISCONNECTED, "jsg.Error: tunneled disconnect"), *locks);
  worker.expectSameTrace(KJ_EXCEPTION(OVERLOADED, "jsg.Error: tunneled overload"), *locks);

  // Exceptions whose details JavaScript does not get to see.
  expectInternal(failure("something broke"));
  expectInternal(failure("remote.broken.outputGateBroken; storage failed"));
  expectInternal(failure(" jsg.TypeError: not at the start"));
  expectInternal(failure("jsg.DOMException: no name"));
  expectInternal(failure("jsg.DOMException(: unclosed"));
  expectInternal(KJ_EXCEPTION(OVERLOADED, "too busy"));
  KJ_EXPECT(expectInternal(failure("jsg-internal.TypeError: secret")).name == "TypeError");
  KJ_EXPECT(expectInternal(failure("jsg-internal.DOMException(OperationError): secret")).name ==
      "OperationError");
  {
    auto traced = worker.expectSameTrace(KJ_EXCEPTION(DISCONNECTED, "peer went away"), *locks);
    KJ_EXPECT(traced.name == "Error");
    KJ_EXPECT(traced.message == "Network connection lost.");
    KJ_EXPECT(traced.stack == kj::none);
  }
  {
    auto traced =
        worker.expectSameTrace(failure("worker_do_not_log; script exceeded time limit"), *locks);
    KJ_EXPECT(traced.message.startsWith(INTERNAL_ERROR_PREFIX), traced.message);
  }
}

KJ_TEST("logUncaughtExceptionAsync() traces what converting the exception to JavaScript shows") {
  expectSameTraces(false);
}

KJ_TEST("logUncaughtExceptionAsync() traces what converting the exception to JavaScript shows, "
        "with enhanced error serialization") {
  expectSameTraces(true);
}

// Turns on INFO logging, which is how exceptions are logged to the console when there is no
// inspector, while it exists.
class VerboseLogging {
 public:
  VerboseLogging(): previousLevel(currentLogLevel()) {
    kj::_::Debug::setLogLevel(kj::LogSeverity::INFO);
  }
  ~VerboseLogging() noexcept(false) {
    kj::_::Debug::setLogLevel(previousLevel);
  }
  KJ_DISALLOW_COPY_AND_MOVE(VerboseLogging);

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

KJ_TEST("logUncaughtExceptionToConsole() logs an exception as it was described") {
  TracedWorker worker;
  auto logToConsole = [&](kj::Exception exception, const jsg::JsErrorDescription& error) {
    worker.fixture.enterContext(*worker.request, [&](const TestFixture::Environment& env) {
      env.lock.logUncaughtExceptionToConsole(
          UncaughtExceptionSource::REQUEST_HANDLER, kj::mv(exception), error);
    });
  };

  {
    // An internal exception is logged once, when it is described, and shows up on the console
    // under the same id.
    auto exception = failure("something broke");
    auto error = [&]() {
      KJ_EXPECT_LOG(ERROR, "something broke; sentryErrorContext = jsgInternalError; wdErrId = ");
      return jsg::describeAsJsError(exception, kj::none);
    }();
    KJ_ASSERT(error.message.startsWith(INTERNAL_ERROR_PREFIX));

    VerboseLogging verbose;
    auto expected = kj::str(
        "uncaught exception; source = Uncaught (in response); stack = Error: ", error.message);
    KJ_EXPECT_LOG(INFO, expected);
    logToConsole(kj::mv(exception), error);
  }

  {
    // One with a type keeps it.
    auto exception = failure("jsg-internal.DOMException(OperationError): secret");
    auto error = [&]() {
      KJ_EXPECT_LOG(ERROR, "jsg-internal.DOMException(OperationError): secret");
      return jsg::describeAsJsError(exception, kj::none);
    }();

    VerboseLogging verbose;
    auto expected = kj::str("stack = OperationError: ", error.message);
    KJ_EXPECT_LOG(INFO, expected);
    logToConsole(kj::mv(exception), error);
  }

  {
    // The error on the console is the one that converting the exception makes, with the
    // properties that say what kind of failure it is, under the id that the exception was logged
    // with when it was described.
    auto exception = KJ_EXCEPTION(OVERLOADED, "remote.broken.outputGateBroken; storage failed");
    jsg::markTransient(exception);
    jsg::addDurableObjectId(exception, "some-durable-object");
    auto error = [&]() {
      KJ_EXPECT_LOG(ERROR, "storage failed; sentryErrorContext = jsgInternalError; wdErrId = ");
      return jsg::describeAsJsError(exception, kj::none);
    }();
    auto id = kj::str(KJ_ASSERT_NONNULL(error.internalErrorId));
    KJ_EXPECT(error.message == kj::str(INTERNAL_ERROR_PREFIX, id));
    KJ_EXPECT(kj::str(KJ_ASSERT_NONNULL(
                  KJ_ASSERT_NONNULL(error.internalException).internalErrorId)) == id);

    // Any ERROR log from here on, such as that of an exception logged a second time, fails the
    // test.
    worker.fixture.enterContext(*worker.request, [&](const TestFixture::Environment& env) {
      // What logUncaughtExceptionToConsole() shows the inspector.
      auto shown = env.js.exceptionToJsValue(
          exception.clone(), {.trusted = true, .alreadyDescribed = error});
      auto object = KJ_ASSERT_NONNULL(shown.getHandle(env.js).tryCast<jsg::JsObject>());
      KJ_EXPECT(kj::str(object.get(env.js, "message")) == error.message);
      KJ_EXPECT(object.get(env.js, "remote").isTrue());
      KJ_EXPECT(object.get(env.js, "retryable").isTrue());
      KJ_EXPECT(object.get(env.js, "overloaded").isTrue());
      KJ_EXPECT(object.get(env.js, "durableObjectReset").isTrue());
      KJ_EXPECT(kj::str(object.get(env.js, "durableObjectId")) == "some-durable-object");
    });

    VerboseLogging verbose;
    auto expected = kj::str("stack = Error: ", INTERNAL_ERROR_PREFIX, id);
    KJ_EXPECT_LOG(INFO, expected);
    logToConsole(kj::mv(exception), error);
  }

  {
    // An exception that is not worth logging still has an id, which the console shows too.
    auto exception = failure("worker_do_not_log; script exceeded time limit");
    auto error = jsg::describeAsJsError(exception, kj::none);
    KJ_EXPECT(KJ_ASSERT_NONNULL(error.internalException).internalErrorId == kj::none);
    auto id = kj::str(KJ_ASSERT_NONNULL(error.internalErrorId));

    VerboseLogging verbose;
    auto expected = kj::str("stack = Error: ", INTERNAL_ERROR_PREFIX, id);
    KJ_EXPECT_LOG(INFO, expected);
    logToConsole(kj::mv(exception), error);
  }

  {
    // A JavaScript error is the original error, with its stack.
    auto exception = worker.tunnelThrown("function thrower() { throw new RangeError('boom'); }\n"
                                         "thrower();");
    auto error = jsg::describeAsJsError(exception, kj::none);

    VerboseLogging verbose;
    KJ_EXPECT_LOG(INFO,
        "uncaught exception; source = Uncaught (in response); "
        "stack = RangeError: boom\n    at thrower (");
    logToConsole(kj::mv(exception), error);
  }

  // The console is not the trace.
  KJ_EXPECT(worker.tracer->exceptions.size() == 0);
}

KJ_TEST("logUncaughtExceptionAsync() logs the exception when verbose") {
  TracedWorker worker;

  {
    auto exception = worker.tunnelThrown("function thrower() { throw new RangeError('boom'); }\n"
                                         "thrower();");
    VerboseLogging verbose;
    KJ_EXPECT_LOG(INFO,
        "uncaught exception; source = Uncaught (in response); "
        "stack = RangeError: boom\n    at thrower (");
    worker.context->logUncaughtExceptionAsync(
        UncaughtExceptionSource::REQUEST_HANDLER, kj::mv(exception), *worker.request);
  }

  {
    // An error without a stack trace is logged by name and message.
    VerboseLogging verbose;
    KJ_EXPECT_LOG(INFO,
        "uncaught exception; source = Uncaught (in response); "
        "stack = TypeError: thrown by C++");
    worker.context->logUncaughtExceptionAsync(UncaughtExceptionSource::REQUEST_HANDLER,
        failure("jsg.TypeError: thrown by C++"), *worker.request);
  }

  KJ_EXPECT(worker.tracer->exceptions.size() == 2);
}

KJ_TEST("logUncaughtExceptionAsync() leaves out a stack that is cut off within the message") {
  TracedWorker worker;
  struct Traces {
    TracedException underLock;
    TracedException withoutLock;
  };
  auto trace = [&](kj::StringPtr code) {
    auto exception = worker.tunnelThrown(code);
    worker.traceUnderLock(exception);
    worker.context->logUncaughtExceptionAsync(
        UncaughtExceptionSource::REQUEST_HANDLER, kj::mv(exception), *worker.request);
    KJ_ASSERT(worker.tracer->exceptions.size() == 2);
    Traces result{kj::mv(worker.tracer->exceptions[0]), kj::mv(worker.tracer->exceptions[1])};
    worker.tracer->exceptions.clear();
    return result;
  };

  {
    // Only 4KB of each property of an error comes along with the exception. For a stack that is
    // not enough to get past a message this long, and the part of the message it does hold is
    // not a stack.
    auto [underLock, withoutLock] =
        trace("function thrower() { throw new Error('y'.repeat(5000)); }\nthrower();");
    KJ_EXPECT(underLock.message.size() == 5000);
    KJ_EXPECT(KJ_ASSERT_NONNULL(underLock.stack).startsWith("    at thrower ("));
    KJ_EXPECT(withoutLock.name == "Error");
    KJ_EXPECT(withoutLock.message == underLock.message.first(4096));
    KJ_EXPECT(withoutLock.stack == kj::none, withoutLock.stack);
  }

  {
    // A stack that is cut off further on keeps the frames it has.
    auto [underLock, withoutLock] = trace("function thrower(depth) {\n"
                                          "  if (depth > 0) return thrower(depth - 1);\n"
                                          "  throw new Error('z'.repeat(3900));\n"
                                          "}\n"
                                          "thrower(8);");
    KJ_EXPECT(withoutLock.message == underLock.message);
    auto& complete = KJ_ASSERT_NONNULL(underLock.stack);
    auto& cutOff = KJ_ASSERT_NONNULL(withoutLock.stack);
    KJ_EXPECT(cutOff.startsWith("    at thrower ("), cutOff);
    KJ_EXPECT(cutOff.size() < complete.size());
    KJ_EXPECT(complete.startsWith(cutOff), cutOff);
  }
}

struct ReportedInternalException {
  kj::String description;
  jsg::InternalExceptionObserver::Detail detail;
};

// Records the internal exceptions reported for a request.
class RequestExceptionRecorder final: public RequestObserver {
 public:
  explicit RequestExceptionRecorder(kj::Vector<ReportedInternalException>& reports)
      : reports(reports) {}

  void reportInternalException(
      const kj::Exception& exception, jsg::InternalExceptionObserver::Detail detail) override {
    reports.add(ReportedInternalException{
      .description = kj::str(exception.getDescription()),
      .detail = detail,
    });
  }

 private:
  kj::Vector<ReportedInternalException>& reports;
};

// Records the internal exceptions reported for an isolate.
class IsolateExceptionRecorder final: public JsgIsolateObserver {
 public:
  void reportInternalException(const kj::Exception& exception, Detail detail) const override {
    reports.lockExclusive()->add(ReportedInternalException{
      .description = kj::str(exception.getDescription()),
      .detail = detail,
    });
  }

  uint count() const {
    return reports.lockExclusive()->size();
  }

  kj::MutexGuarded<kj::Vector<ReportedInternalException>> reports;
};

KJ_TEST("logUncaughtExceptionAsync() reports an internal exception to the isolate's observer "
        "and the request's, once") {
  auto isolateReports = kj::atomicRefcounted<IsolateExceptionRecorder>();
  kj::Vector<ReportedInternalException> requestReports;
  TestFixture::SetupParams params;
  params.jsgIsolateObserver = kj::atomicAddRef(*isolateReports);
  params.requestObserverFactory = [&requestReports]() -> kj::Own<RequestObserver> {
    return kj::refcounted<RequestExceptionRecorder>(requestReports);
  };
  TracedWorker worker(kj::mv(params));
  auto log = [&](kj::Exception exception) {
    worker.context->logUncaughtExceptionAsync(
        UncaughtExceptionSource::REQUEST_HANDLER, kj::mv(exception), *worker.request);
  };

  // The observer that is reachable without the isolate lock is the one that a conversion under
  // the lock reports to.
  const jsg::IsolateObserver& isolateObserver = *isolateReports;
  auto& api = worker.context->getWorker().getIsolate().getApi();
  KJ_EXPECT(&api.getObserver() == &isolateObserver);
  worker.fixture.enterContext(*worker.request, [&](const TestFixture::Environment& env) {
    KJ_EXPECT(&jsg::IsolateBase::from(env.isolate).getObserver() == &isolateObserver);
  });

  {
    // Both reports have the id that the exception is logged under and that the trace refers to.
    KJ_EXPECT_LOG(ERROR, "storage failed; sentryErrorContext = jsgInternalError; wdErrId = ");
    log(failure("remote.broken.outputGateBroken; storage failed"));
    KJ_ASSERT(requestReports.size() == 1);
    KJ_EXPECT(requestReports[0].description == "remote.broken.outputGateBroken; storage failed");
    KJ_EXPECT(requestReports[0].detail.isInternal);
    KJ_EXPECT(requestReports[0].detail.isFromRemote);
    KJ_EXPECT(requestReports[0].detail.isDurableObjectReset);
    auto id = kj::str(KJ_ASSERT_NONNULL(requestReports[0].detail.internalErrorId));
    KJ_ASSERT(worker.tracer->exceptions.size() == 1);
    KJ_EXPECT(worker.tracer->exceptions[0].message == kj::str(INTERNAL_ERROR_PREFIX, id));

    auto reports = isolateReports->reports.lockExclusive();
    KJ_ASSERT(reports->size() == 1);
    auto& report = (*reports)[0];
    KJ_EXPECT(report.description == "remote.broken.outputGateBroken; storage failed");
    KJ_EXPECT(report.detail.isInternal);
    KJ_EXPECT(report.detail.isFromRemote);
    KJ_EXPECT(report.detail.isDurableObjectReset);
    KJ_EXPECT(kj::str(KJ_ASSERT_NONNULL(report.detail.internalErrorId)) == id);
  }

  {
    // A disconnection is reported without an id, as it is not logged under one.
    log(KJ_EXCEPTION(DISCONNECTED, "peer went away"));
    KJ_ASSERT(requestReports.size() == 2);
    KJ_EXPECT(requestReports[1].detail.isInternal);
    KJ_EXPECT(!requestReports[1].detail.isFromRemote);
    KJ_EXPECT(!requestReports[1].detail.isDurableObjectReset);
    KJ_EXPECT(requestReports[1].detail.internalErrorId == kj::none);
    KJ_EXPECT(isolateReports->count() == 2);
  }

  {
    // The current request's observer is told when no request is given.
    KJ_EXPECT_LOG(ERROR, "something broke");
    worker.context->logUncaughtExceptionAsync(
        UncaughtExceptionSource::ASYNC_TASK, failure("something broke"), kj::none);
    KJ_ASSERT(requestReports.size() == 3);
    KJ_EXPECT(requestReports[2].description == "something broke");
    KJ_EXPECT(isolateReports->count() == 3);
  }

  {
    // An exception that was reported when it was described is not reported again when it is
    // converted for the console, which is what happens to it with an inspector attached.
    auto exception = failure("described first");
    auto error = [&]() {
      KJ_EXPECT_LOG(ERROR, "described first");
      return jsg::describeAsJsError(exception, api.getObserver());
    }();
    KJ_EXPECT(isolateReports->count() == 4);
    worker.fixture.enterContext(*worker.request, [&](const TestFixture::Environment& env) {
      env.lock.logUncaughtExceptionToConsole(
          UncaughtExceptionSource::REQUEST_HANDLER, kj::mv(exception), error);
    });
    KJ_EXPECT(isolateReports->count() == 4);

    // Whereas any other conversion does report it.
    worker.fixture.enterContext(*worker.request, [&](const TestFixture::Environment& env) {
      KJ_EXPECT_LOG(ERROR, "converted only");
      env.js.exceptionToJsValue(failure("converted only"));
    });
    KJ_EXPECT(isolateReports->count() == 5);
  }

  // An error that JavaScript gets to see in full is not an internal exception.
  log(failure("jsg.TypeError: thrown by C++"));
  log(worker.tunnelThrown("throw new Error('boom');"));
  KJ_EXPECT(requestReports.size() == 3);
  KJ_EXPECT(isolateReports->count() == 5);
  KJ_EXPECT(worker.tracer->exceptions.size() == 5);

  {
    // Without a tracer or an inspector, an internal exception is logged and not reported.
    auto context = worker.fixture.newIoContext();
    auto request = worker.fixture.newIncomingRequest(*context);
    KJ_EXPECT_LOG(ERROR, "nobody watching; sentryErrorContext = jsgInternalError");
    context->logUncaughtExceptionAsync(
        UncaughtExceptionSource::REQUEST_HANDLER, failure("nobody watching"), *request);
    KJ_EXPECT(requestReports.size() == 3);
    KJ_EXPECT(isolateReports->count() == 5);
    worker.fixture.drainAndDestroy(kj::mv(request));
  }
}

KJ_TEST("logUncaughtExceptionAsync() traces under the given request, or the current one") {
  // Each request has its own clock.
  SyncedTimerChannel olderTimer(START_TIME);
  SyncedTimerChannel newerTimer(START_TIME + 1 * kj::HOURS);
  uint requestCount = 0;
  TestFixture::SetupParams params{
    .actorId = Worker::Actor::Id(kj::str("uncaught-exception-test")),
  };
  params.ioChannelFactory = [&](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(
        requestCount++ == 0 ? olderTimer : newerTimer);
  };
  TestFixture fixture(kj::mv(params));
  auto context = fixture.newIoContext();
  auto newRequest = [&](kj::Rc<ExceptionTracer>& tracer) {
    auto request =
        fixture.newUndeliveredIncomingRequest(*context, kj::Rc<BaseTracer>(tracer.addRef()));
    request->delivered();
    return request;
  };
  auto olderTracer = kj::rc<ExceptionTracer>();
  auto older = newRequest(olderTracer);
  auto newerTracer = kj::rc<ExceptionTracer>();
  auto newer = newRequest(newerTracer);
  olderTimer.actual += 1 * kj::SECONDS;
  newerTimer.actual += 1 * kj::SECONDS;

  context->logUncaughtExceptionAsync(
      UncaughtExceptionSource::REQUEST_HANDLER, failure("jsg.Error: for the older"), *older);
  context->logUncaughtExceptionAsync(
      UncaughtExceptionSource::ASYNC_TASK, failure("jsg.Error: for the current"), kj::none);
  // An intermediate report is left out of the trace.
  context->logUncaughtExceptionAsync(
      UncaughtExceptionSource::INTERNAL_ASYNC, failure("jsg.Error: intermediate"), *newer);

  KJ_ASSERT(olderTracer->exceptions.size() == 1);
  KJ_EXPECT(olderTracer->exceptions[0].message == "for the older");
  KJ_EXPECT(olderTracer->exceptions[0].spanContext == kj::str(older->getInvocationSpanContext()));
  KJ_EXPECT(olderTracer->exceptions[0].timestamp == olderTimer.actual);
  KJ_ASSERT(newerTracer->exceptions.size() == 1);
  KJ_EXPECT(newerTracer->exceptions[0].message == "for the current");
  KJ_EXPECT(newerTracer->exceptions[0].spanContext == kj::str(newer->getInvocationSpanContext()));
  KJ_EXPECT(newerTracer->exceptions[0].timestamp == newerTimer.actual);

  fixture.drainAndDestroy(kj::mv(newer));
  fixture.drainAndDestroy(kj::mv(older));
}

}  // namespace
}  // namespace workerd
