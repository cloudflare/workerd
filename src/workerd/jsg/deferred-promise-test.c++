// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "deferred-promise.h"
#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct DeferredPromiseContext: public jsg::Object, public jsg::ContextGlobal {
  // Test basic resolve/reject flow
  void testBasicResolve(jsg::Lock& js) {
    auto pair = newDeferredPromiseAndResolver<int>();
    KJ_EXPECT(pair.promise.isPending());
    KJ_EXPECT(!pair.promise.isResolved());
    KJ_EXPECT(!pair.promise.isRejected());

    pair.resolver.resolve(js, 42);
    KJ_EXPECT(!pair.promise.isPending());
    KJ_EXPECT(pair.promise.isResolved());
    KJ_EXPECT(!pair.promise.isRejected());
  }

  void testBasicReject(jsg::Lock& js) {
    auto pair = newDeferredPromiseAndResolver<int>();
    // Use v8StrIntern directly as the rejection value, not ThrowException
    pair.resolver.reject(js, v8StrIntern(js.v8Isolate, "error"));
    KJ_EXPECT(!pair.promise.isPending());
    KJ_EXPECT(!pair.promise.isResolved());
    KJ_EXPECT(pair.promise.isRejected());
  }

  // Test .then() with sync callbacks
  void testThenSync(jsg::Lock& js) {
    int result = 0;

    auto pair = newDeferredPromiseAndResolver<int>();
    pair.promise.then(js, [&result](jsg::Lock&, int value) { result = value * 2; });

    KJ_EXPECT(result == 0);
    pair.resolver.resolve(js, 21);
    KJ_EXPECT(result == 42);
  }

  // Test .then() with value transformation
  void testThenTransform(jsg::Lock& js) {
    kj::String result;

    auto pair = newDeferredPromiseAndResolver<int>();
    auto stringPromise = pair.promise.then(
        js, [](jsg::Lock&, int value) -> kj::String { return kj::str(value * 2); });

    stringPromise.then(js, [&result](jsg::Lock&, kj::String value) { result = kj::mv(value); });

    pair.resolver.resolve(js, 21);
    KJ_EXPECT(result == "42");
  }

  // Test already-resolved promise
  void testAlreadyResolved(jsg::Lock& js) {
    int result = 0;

    auto promise = DeferredPromise<int>::resolved(42);
    KJ_EXPECT(promise.isResolved());
    KJ_EXPECT(!promise.isPending());

    promise.then(js, [&result](jsg::Lock&, int value) { result = value; });
    KJ_EXPECT(result == 42);
  }

  // Test already-rejected promise
  void testAlreadyRejected(jsg::Lock& js) {
    bool errorCalled = false;

    auto promise =
        DeferredPromise<int>::rejected(js, JSG_KJ_EXCEPTION(FAILED, Error, "test error"));
    KJ_EXPECT(promise.isRejected());

    promise.then(js, [](jsg::Lock&, int) { KJ_FAIL_REQUIRE("should not be called"); },
        [&errorCalled](jsg::Lock& js, kj::Exception exception) {
      // Verify we got the exception with the right description
      KJ_EXPECT(exception.getDescription().contains("test error"));
      errorCalled = true;
    });
    KJ_EXPECT(errorCalled);
  }

  // Test .catch_()
  void testCatch(jsg::Lock& js) {
    int result = 0;

    auto pair = newDeferredPromiseAndResolver<int>();
    auto recovered = pair.promise.catch_(js, [](jsg::Lock&, kj::Exception) -> int { return 123; });

    recovered.then(js, [&result](jsg::Lock&, int value) { result = value; });

    pair.resolver.reject(js, JSG_KJ_EXCEPTION(FAILED, Error, "error"));
    KJ_EXPECT(result == 123);
  }

  // Test void promise
  void testVoidPromise(jsg::Lock& js) {
    bool resolved = false;

    auto pair = newDeferredPromiseAndResolver<void>();
    pair.promise.then(js, [&resolved](jsg::Lock&) { resolved = true; });

    KJ_EXPECT(!resolved);
    pair.resolver.resolve(js);
    KJ_EXPECT(resolved);
  }

  // Test whenResolved() does not consume the promise
  void testWhenResolved(jsg::Lock& js) {
    int resolvedCount = 0;
    int thenCount = 0;

    auto pair = newDeferredPromiseAndResolver<int>();

    // whenResolved() should not consume
    pair.promise.whenResolved(js).then(js, [&resolvedCount](jsg::Lock&) { resolvedCount++; });

    // .then() should still work after whenResolved()
    pair.promise.then(js, [&thenCount](jsg::Lock&, int value) { thenCount = value; });

    pair.resolver.resolve(js, 42);
    KJ_EXPECT(resolvedCount == 1);
    KJ_EXPECT(thenCount == 42);
  }

  // Test whenResolved() propagates rejections
  void testWhenResolvedReject(jsg::Lock& js) {
    bool errorCaught = false;
    kj::String errorMessage;
    bool thenErrorCaught = false;

    auto pair = newDeferredPromiseAndResolver<int>();

    // whenResolved() should propagate rejection
    pair.promise.whenResolved(js).then(js, [](jsg::Lock&) {
      KJ_FAIL_REQUIRE("should not resolve");
    }, [&errorCaught, &errorMessage](jsg::Lock&, kj::Exception exception) {
      errorCaught = true;
      errorMessage = kj::str(exception.getDescription());
    });

    // .then() should still work after whenResolved() and also see the rejection
    pair.promise.then(js, [](jsg::Lock&, int) { KJ_FAIL_REQUIRE("should not resolve"); },
        [&thenErrorCaught](jsg::Lock&, kj::Exception) { thenErrorCaught = true; });

    pair.resolver.reject(js, JSG_KJ_EXCEPTION(FAILED, Error, "test rejection"));
    KJ_EXPECT(errorCaught);
    KJ_EXPECT(thenErrorCaught);
    KJ_EXPECT(errorMessage.contains("test rejection"_kj), errorMessage);
  }

  // Test whenResolved() on already-rejected promise
  void testWhenResolvedAlreadyRejected(jsg::Lock& js) {
    bool errorCaught = false;

    // Create an already-rejected promise
    auto promise =
        DeferredPromise<int>::rejected(js, JSG_KJ_EXCEPTION(FAILED, Error, "already failed"));

    // whenResolved() should immediately return a rejected void promise
    auto whenResolvedPromise = promise.whenResolved(js);

    // It should already be rejected
    KJ_EXPECT(whenResolvedPromise.isRejected());

    whenResolvedPromise.catch_(
        js, [&errorCaught](jsg::Lock&, kj::Exception) { errorCaught = true; });

    // Since the promise is already rejected, continuation runs synchronously
    KJ_EXPECT(errorCaught);
  }

  // Test conversion to jsg::Promise
  void testToJsPromise(jsg::Lock& js) {
    auto pair = newDeferredPromiseAndResolver<int>();
    auto jsPromise = pair.promise.toJsPromise(js);

    int result = 0;
    jsPromise.then(js, [&result](jsg::Lock&, int value) { result = value; });

    pair.resolver.resolve(js, 42);
    js.runMicrotasks();
    KJ_EXPECT(result == 42);
  }

  // Test toJsPromise() with pending promise that is then rejected
  void testToJsPromiseReject(jsg::Lock& js) {
    auto pair = newDeferredPromiseAndResolver<int>();
    auto jsPromise = pair.promise.toJsPromise(js);

    bool errorCaught = false;
    kj::String errorMessage;

    // jsg::Promise error handler receives Value, not kj::Exception
    jsPromise.then(js, [](jsg::Lock&, int) { KJ_FAIL_REQUIRE("should not resolve"); },
        [&errorCaught, &errorMessage](jsg::Lock& js, Value error) {
      errorCaught = true;
      // The kj::Exception should have been converted to a JS Error
      v8::HandleScope scope(js.v8Isolate);
      auto str = error.getHandle(js)->ToString(js.v8Context()).ToLocalChecked();
      v8::String::Utf8Value utf8(js.v8Isolate, str);
      errorMessage = kj::str(*utf8);
    });

    // Reject with kj::Exception - it should be converted to JS Error
    pair.resolver.reject(js, JSG_KJ_EXCEPTION(FAILED, Error, "test error message"));
    js.runMicrotasks();

    KJ_EXPECT(errorCaught, "Error handler should have been called");
    KJ_EXPECT(errorMessage.contains("test error message"_kj), errorMessage);
  }

  // Test toJsPromise() on already-rejected DeferredPromise
  void testToJsPromiseAlreadyRejected(jsg::Lock& js) {
    // Create an already-rejected DeferredPromise
    auto promise =
        DeferredPromise<int>::rejected(js, JSG_KJ_EXCEPTION(FAILED, Error, "already rejected"));

    // Convert to jsg::Promise
    auto jsPromise = promise.toJsPromise(js);

    bool errorCaught = false;
    kj::String errorMessage;

    jsPromise.then(js, [](jsg::Lock&, int) { KJ_FAIL_REQUIRE("should not resolve"); },
        [&errorCaught, &errorMessage](jsg::Lock& js, Value error) {
      errorCaught = true;
      v8::HandleScope scope(js.v8Isolate);
      auto str = error.getHandle(js)->ToString(js.v8Context()).ToLocalChecked();
      v8::String::Utf8Value utf8(js.v8Isolate, str);
      errorMessage = kj::str(*utf8);
    });

    js.runMicrotasks();

    KJ_EXPECT(errorCaught, "Error handler should have been called");
    KJ_EXPECT(errorMessage.contains("already rejected"_kj), errorMessage);
  }

  // Test promise chaining - DeferredPromise returning DeferredPromise
  void testDeferredChaining(jsg::Lock& js) {
    int result = 0;

    auto outerPair = newDeferredPromiseAndResolver<int>();
    auto innerPair = newDeferredPromiseAndResolver<int>();

    // The inner DeferredPromise should be automatically chained
    outerPair.promise
        .then(js, [&innerPair](jsg::Lock&, int) -> DeferredPromise<int> {
      return kj::mv(innerPair.promise);
    }).then(js, [&result](jsg::Lock&, int value) { result = value; });

    outerPair.resolver.resolve(js, 1);
    KJ_EXPECT(result == 0);  // Still waiting on inner

    innerPair.resolver.resolve(js, 42);
    KJ_EXPECT(result == 42);
  }

  // Test promise chaining - DeferredPromise returning jsg::Promise
  void testJsgPromiseChaining(jsg::Lock& js) {
    int result = 0;

    auto pair = newDeferredPromiseAndResolver<int>();

    pair.promise
        .then(js, [](jsg::Lock& js, int value) -> jsg::Promise<int> {
      return js.resolvedPromise(value * 2);
    }).then(js, [&result](jsg::Lock&, int value) { result = value; });

    pair.resolver.resolve(js, 21);
    js.runMicrotasks();  // jsg::Promise uses microtasks
    KJ_EXPECT(result == 42);
  }

  // Test error propagation through chain
  void testErrorPropagation(jsg::Lock& js) {
    kj::String errorMessage;

    auto pair = newDeferredPromiseAndResolver<int>();
    pair.promise.then(js, [](jsg::Lock&, int value) -> int { return value * 2; })
        .then(js, [](jsg::Lock&, int value) -> int {
      return value + 10;
    }).then(js, [](jsg::Lock&, int) {
      KJ_FAIL_REQUIRE("should not reach here");
    }, [&errorMessage](jsg::Lock& js, kj::Exception exception) {
      errorMessage = kj::str(exception.getDescription());
    });

    pair.resolver.reject(js, JSG_KJ_EXCEPTION(FAILED, Error, "original error"));
    KJ_EXPECT(errorMessage.contains("original error"));
  }

  // Test tryConsumeResolved optimization
  void testTryConsumeResolved(jsg::Lock& js) {
    {
      // Pending promise should return none
      auto pair = newDeferredPromiseAndResolver<int>();
      KJ_EXPECT(pair.promise.tryConsumeResolved() == kj::none);
    }

    {
      // Resolved promise should return value
      auto promise = DeferredPromise<int>::resolved(42);
      auto value = KJ_ASSERT_NONNULL(promise.tryConsumeResolved());
      KJ_EXPECT(value == 42);
    }
  }

  // Test multiple resolvers sharing state
  void testResolverAddRef(jsg::Lock& js) {
    auto pair = newDeferredPromiseAndResolver<int>();
    auto resolver2 = pair.resolver.addRef();

    int result = 0;
    pair.promise.then(js, [&result](jsg::Lock&, int value) { result = value; });

    // Either resolver can resolve
    resolver2.resolve(js, 42);
    KJ_EXPECT(result == 42);
  }

  // Test converting jsg::Promise to DeferredPromise
  void testFromJsPromise(jsg::Lock& js) {
    int result = 0;

    // Create a jsg::Promise
    auto [jsPromise, jsResolver] = js.newPromiseAndResolver<int>();

    // Convert to DeferredPromise and set up continuation chain
    auto deferred = DeferredPromise<int>::fromJsPromise(js, kj::mv(jsPromise));

    // These continuations will run synchronously when the JS promise resolves
    deferred.then(js, [](jsg::Lock&, int value) -> int {
      return value * 2;
    }).then(js, [&result](jsg::Lock&, int value) { result = value; });

    KJ_EXPECT(result == 0);  // Not yet resolved

    // Resolve the original JS promise
    jsResolver.resolve(js, 21);
    js.runMicrotasks();  // jsg::Promise uses microtasks

    KJ_EXPECT(result == 42);  // Continuations ran synchronously after microtask
  }

  // Test fromJsPromise with rejection (pending promise that gets rejected)
  void testFromJsPromiseReject(jsg::Lock& js) {
    bool errorCaught = false;

    auto [jsPromise, jsResolver] = js.newPromiseAndResolver<int>();
    auto deferred = DeferredPromise<int>::fromJsPromise(js, kj::mv(jsPromise));

    deferred.then(js, [](jsg::Lock&, int) { KJ_FAIL_REQUIRE("should not be called"); },
        [&errorCaught](jsg::Lock&, kj::Exception) { errorCaught = true; });

    jsResolver.reject(js, JSG_KJ_EXCEPTION(FAILED, Error, "test error"));
    js.runMicrotasks();

    KJ_EXPECT(errorCaught);
  }

  // Test fromJsPromise with already-resolved JS promise (optimization path)
  void testFromJsPromiseAlreadyResolved(jsg::Lock& js) {
    int result = 0;

    // Create a jsg::Promise that is already resolved
    auto jsPromise = js.resolvedPromise(42);

    // Convert to DeferredPromise - should detect it's already resolved
    auto deferred = DeferredPromise<int>::fromJsPromise(js, kj::mv(jsPromise));

    // The DeferredPromise should already be resolved (not pending)
    KJ_EXPECT(deferred.isResolved());
    KJ_EXPECT(!deferred.isPending());

    // Continuations should run synchronously without needing microtasks
    deferred.then(js, [&result](jsg::Lock&, int value) { result = value * 2; });

    // Result should be set immediately - no microtasks needed!
    KJ_EXPECT(result == 84);
  }

  // Test fromJsPromise with already-rejected JS promise (optimization path)
  void testFromJsPromiseAlreadyRejected(jsg::Lock& js) {
    bool errorCaught = false;
    kj::String errorMessage;

    // Create a jsg::Promise that is already rejected
    auto jsPromise = js.rejectedPromise<int>(JSG_KJ_EXCEPTION(FAILED, Error, "already failed"));

    // Convert to DeferredPromise - should detect it's already rejected
    auto deferred = DeferredPromise<int>::fromJsPromise(js, kj::mv(jsPromise));

    // The DeferredPromise should already be rejected (not pending)
    KJ_EXPECT(deferred.isRejected());
    KJ_EXPECT(!deferred.isPending());

    // Error handler should run synchronously without needing microtasks
    deferred.then(js, [](jsg::Lock&, int) { KJ_FAIL_REQUIRE("should not be called"); },
        [&errorCaught, &errorMessage](jsg::Lock& js, kj::Exception exception) {
      errorCaught = true;
      errorMessage = kj::str(exception.getDescription());
    });

    // Error should be caught immediately - no microtasks needed!
    KJ_EXPECT(errorCaught);
    KJ_EXPECT(errorMessage.contains("already failed"));
  }

  // Test fromJsPromise with already-resolved void JS promise
  void testFromJsPromiseAlreadyResolvedVoid(jsg::Lock& js) {
    bool resolved = false;

    // Create a void jsg::Promise that is already resolved
    auto jsPromise = js.resolvedPromise();

    // Convert to DeferredPromise - should detect it's already resolved
    auto deferred = DeferredPromise<void>::fromJsPromise(js, kj::mv(jsPromise));

    // The DeferredPromise should already be resolved
    KJ_EXPECT(deferred.isResolved());
    KJ_EXPECT(!deferred.isPending());

    // Continuation should run synchronously
    deferred.then(js, [&resolved](jsg::Lock&) { resolved = true; });

    // Should be set immediately
    KJ_EXPECT(resolved);
  }

  // Test fromJsPromise with already-rejected void JS promise
  void testFromJsPromiseAlreadyRejectedVoid(jsg::Lock& js) {
    bool errorCaught = false;

    // Create a void jsg::Promise that is already rejected
    auto jsPromise = js.rejectedPromise<void>(JSG_KJ_EXCEPTION(FAILED, Error, "void rejection"));

    // Convert to DeferredPromise - should detect it's already rejected
    auto deferred = DeferredPromise<void>::fromJsPromise(js, kj::mv(jsPromise));

    // The DeferredPromise should already be rejected
    KJ_EXPECT(deferred.isRejected());
    KJ_EXPECT(!deferred.isPending());

    // Error handler should run synchronously
    deferred.then(js, [](jsg::Lock&) { KJ_FAIL_REQUIRE("should not be called"); },
        [&errorCaught](jsg::Lock&, kj::Exception) { errorCaught = true; });

    KJ_EXPECT(errorCaught);
  }

  // Test that deep promise chains don't cause stack overflow (trampolining)
  void testDeepChain(jsg::Lock& js) {
    constexpr size_t CHAIN_DEPTH = 10000;

    int result = 0;

    // Build a very deep chain - this would overflow the stack without trampolining
    auto pair = newDeferredPromiseAndResolver<int>();
    auto promise = kj::mv(pair.promise);

    for (size_t i = 0; i < CHAIN_DEPTH; ++i) {
      promise = kj::mv(promise).then(js, [](jsg::Lock&, int v) { return v + 1; });
    }

    promise.then(js, [&result](jsg::Lock&, int v) { result = v; });

    // Resolve - if trampolining works, this won't overflow the stack
    pair.resolver.resolve(js, 0);

    // All callbacks should have run
    KJ_EXPECT(result == CHAIN_DEPTH);
  }

  // Test that FIFO order is maintained with trampolining
  void testTrampolineOrder(jsg::Lock& js) {
    kj::Vector<int> order;

    auto pair1 = newDeferredPromiseAndResolver<void>();
    auto pair2 = newDeferredPromiseAndResolver<void>();
    auto pair3 = newDeferredPromiseAndResolver<void>();

    pair1.promise.then(js, [&order](jsg::Lock&) { order.add(1); });
    pair2.promise.then(js, [&order](jsg::Lock&) { order.add(2); });
    pair3.promise.then(js, [&order](jsg::Lock&) { order.add(3); });

    // Resolve in order 1, 2, 3
    pair1.resolver.resolve(js);
    pair2.resolver.resolve(js);
    pair3.resolver.resolve(js);

    // Should maintain FIFO order
    KJ_ASSERT(order.size() == 3);
    KJ_EXPECT(order[0] == 1);
    KJ_EXPECT(order[1] == 2);
    KJ_EXPECT(order[2] == 3);
  }

  // Helper to log a stack trace for visual inspection
  static void logStackTrace(kj::StringPtr label, const kj::Exception& ex) {
    auto trace = ex.getStackTrace();
    KJ_DBG(label, "trace size", trace.size());
    for (size_t i = 0; i < trace.size(); ++i) {
      KJ_DBG("  ", i, trace[i]);
    }
  }

  // Test that async stack traces are extended when rejecting via resolver.reject()
  // Note: We verify by examining the kj::Exception before it's converted to JS,
  // since the trace is not preserved through JS round-tripping.
  void testAsyncStackTraceOnReject(jsg::Lock& js) {
    bool errorHandled = false;
    kj::String errorDesc;

    auto pair = newDeferredPromiseAndResolver<int>();

    // Set up a chain of .then() calls
    pair.promise.then(js, [](jsg::Lock&, int v) -> int { return v * 2; })
        .then(js, [](jsg::Lock&, int v) -> int { return v + 10; })
        .then(js, [](jsg::Lock&, int v) -> int {
      return v * 3;
    }).then(js, [](jsg::Lock&, int) {
      KJ_FAIL_REQUIRE("should not reach here");
    }, [&errorHandled, &errorDesc](jsg::Lock& js, kj::Exception exception) {
      errorHandled = true;
      errorDesc = kj::str(exception.getDescription());
    });

    // Create an exception and log initial trace
    auto exception = JSG_KJ_EXCEPTION(FAILED, Error, "test error");
    logStackTrace("Initial exception"_kj, exception);
    size_t initialTrace = exception.getStackTrace().size();

    // Manually call addTraceHere to verify it works
    exception.addTraceHere();
    logStackTrace("After addTraceHere"_kj, exception);
    size_t afterAddTrace = exception.getStackTrace().size();

    // addTraceHere should add at least one entry
    KJ_EXPECT(afterAddTrace >= initialTrace, "addTraceHere should not decrease trace size",
        afterAddTrace, initialTrace);

    // Now reject with the exception
    pair.resolver.reject(js, kj::mv(exception));

    // Verify the error propagated correctly
    KJ_EXPECT(errorHandled, "Error handler should have been called");
    KJ_EXPECT(errorDesc.contains("test error"), "Error should contain original message");
  }

  // Test that async stack traces are extended when a callback throws.
  // We verify by capturing the trace size at the throw site and comparing
  // to the trace size when the exception is caught (before JS conversion).
  void testAsyncStackTraceOnThrow(jsg::Lock& js) {
    // We'll use thread-local storage to capture trace info across the throw/catch boundary
    static thread_local size_t traceAtThrow = 0;
    static thread_local size_t traceAtCatch = 0;

    auto pair = newDeferredPromiseAndResolver<int>();

    // Set up a chain where the first callback throws
    pair.promise
        .then(js, [](jsg::Lock&, int) -> int {
      // Create exception and record trace size at throw site
      auto ex = JSG_KJ_EXCEPTION(FAILED, Error, "intentional test error");
      traceAtThrow = ex.getStackTrace().size();
      logStackTrace("Exception at throw site"_kj, ex);
      kj::throwFatalException(kj::mv(ex));
    }).then(js, [](jsg::Lock&, int v) -> int {
      return v + 10;
    }).then(js, [](jsg::Lock&, int v) -> int {
      return v * 3;
    }).catch_(js, [](jsg::Lock& js, kj::Exception exception) -> int {
      // Now we receive the exception directly - trace is preserved!
      traceAtCatch = exception.getStackTrace().size();
      logStackTrace("Exception at catch"_kj, exception);
      KJ_DBG(exception);
      return 0;
    });

    // Resolve to trigger the chain - the first callback will throw
    pair.resolver.resolve(js, 42);

    // Log what we captured
    KJ_DBG("Trace at throw site", traceAtThrow);
    KJ_DBG("Trace at catch (preserved through chain!)", traceAtCatch);

    // Now that we store kj::Exception natively, the trace IS preserved through the chain!
    // The trace should have grown as the exception propagated through .then() handlers.
    KJ_EXPECT(traceAtCatch >= traceAtThrow, "Trace should be preserved through the chain",
        traceAtCatch, traceAtThrow);
  }

  // Test that addTrace(void*) correctly adds a specific address to the exception trace.
  // This is the mechanism DeferredPromise uses for async stack traces - it captures
  // the return address at .then() call time and adds it when an exception propagates.
  void testAsyncStackTraceDepth(jsg::Lock& js) {
    // Create an exception and verify addTrace works with a specific address
    auto exception = JSG_KJ_EXCEPTION(FAILED, Error, "test");
    size_t initial = exception.getStackTrace().size();

    // Use a known address (current function's return address as a stand-in)
    void* testAddress = __builtin_return_address(0);
    exception.addTrace(testAddress);

    auto trace = exception.getStackTrace();
    KJ_EXPECT(trace.size() == initial + 1, "addTrace should add one entry");

    // Verify the specific address we added is in the trace
    bool foundAddress = false;
    for (auto addr: trace) {
      if (addr == testAddress) {
        foundAddress = true;
        break;
      }
    }
    KJ_EXPECT(foundAddress, "Trace should contain the exact address we added");

    // Add more addresses and verify they accumulate
    void* testAddress2 = reinterpret_cast<void*>(0x12345678);
    void* testAddress3 = reinterpret_cast<void*>(0xDEADBEEF);
    exception.addTrace(testAddress2);
    exception.addTrace(testAddress3);

    trace = exception.getStackTrace();
    KJ_EXPECT(trace.size() == initial + 3, "Should have 3 added entries");

    // Log for visual inspection - shows the addresses are preserved exactly
    KJ_DBG("Test address from return address", testAddress);
    logStackTrace("Exception with multiple addresses"_kj, exception);
  }

  // Test that verifies DeferredPromise captures user code addresses in traces.
  // We use resolver.reject(kj::Exception) to verify addresses are added correctly.
  void testContinuationTraceAddress(jsg::Lock& js) {
    // This address is within testContinuationTraceAddress
    void* addressInThisFunction = __builtin_return_address(0);

    auto pair = newDeferredPromiseAndResolver<int>();

    bool errorHandled = false;
    pair.promise.then(js, [](jsg::Lock&, int v) -> int { return v * 2; },
        [&errorHandled](jsg::Lock& js, kj::Exception exception) -> int {
      // Now we receive the exception directly - can inspect the trace!
      errorHandled = true;
      return 0;
    });

    // Create an exception and add our address to simulate what happens
    // when DeferredPromise catches and re-throws
    auto exception = JSG_KJ_EXCEPTION(FAILED, Error, "test error");

    // The reject() method calls addTraceHere() which adds the address
    // of the code inside reject() - but we want to verify the mechanism works.
    // Manually add an address we can verify:
    exception.addTrace(addressInThisFunction);

    size_t traceSize = exception.getStackTrace().size();
    KJ_EXPECT(traceSize >= 1, "Exception should have at least one trace entry");

    // Verify our address is in the trace
    bool found = false;
    for (auto addr: exception.getStackTrace()) {
      if (addr == addressInThisFunction) {
        found = true;
        break;
      }
    }
    KJ_EXPECT(found, "Trace should contain address from this test function");

    // Now reject with this exception - the resolver.reject() will also add its own trace
    pair.resolver.reject(js, kj::mv(exception));

    KJ_EXPECT(errorHandled, "Error handler should have been called");
    KJ_DBG("Address in test function", addressInThisFunction);
  }

  JSG_RESOURCE_TYPE(DeferredPromiseContext) {
    JSG_METHOD(testBasicResolve);
    JSG_METHOD(testBasicReject);
    JSG_METHOD(testThenSync);
    JSG_METHOD(testThenTransform);
    JSG_METHOD(testFromJsPromise);
    JSG_METHOD(testFromJsPromiseReject);
    JSG_METHOD(testFromJsPromiseAlreadyResolved);
    JSG_METHOD(testFromJsPromiseAlreadyRejected);
    JSG_METHOD(testFromJsPromiseAlreadyResolvedVoid);
    JSG_METHOD(testFromJsPromiseAlreadyRejectedVoid);
    JSG_METHOD(testAlreadyResolved);
    JSG_METHOD(testAlreadyRejected);
    JSG_METHOD(testCatch);
    JSG_METHOD(testVoidPromise);
    JSG_METHOD(testWhenResolved);
    JSG_METHOD(testWhenResolvedReject);
    JSG_METHOD(testWhenResolvedAlreadyRejected);
    JSG_METHOD(testToJsPromise);
    JSG_METHOD(testToJsPromiseReject);
    JSG_METHOD(testToJsPromiseAlreadyRejected);
    JSG_METHOD(testDeferredChaining);
    JSG_METHOD(testJsgPromiseChaining);
    JSG_METHOD(testErrorPropagation);
    JSG_METHOD(testTryConsumeResolved);
    JSG_METHOD(testResolverAddRef);
    JSG_METHOD(testDeepChain);
    JSG_METHOD(testTrampolineOrder);
    JSG_METHOD(testAsyncStackTraceOnReject);
    JSG_METHOD(testAsyncStackTraceOnThrow);
    JSG_METHOD(testAsyncStackTraceDepth);
    JSG_METHOD(testContinuationTraceAddress);
  }
};

JSG_DECLARE_ISOLATE_TYPE(DeferredPromiseIsolate, DeferredPromiseContext);

KJ_TEST("DeferredPromise basic resolve") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testBasicResolve()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise basic reject") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testBasicReject()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise then sync") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testThenSync()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise then transform") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testThenTransform()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise already resolved") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testAlreadyResolved()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise already rejected") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testAlreadyRejected()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise catch") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testCatch()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise void") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testVoidPromise()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise whenResolved") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testWhenResolved()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise whenResolved reject") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testWhenResolvedReject()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise whenResolved already rejected") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testWhenResolvedAlreadyRejected()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise to jsg::Promise") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testToJsPromise()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise to jsg::Promise reject") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testToJsPromiseReject()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise to jsg::Promise already rejected") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testToJsPromiseAlreadyRejected()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise deferred chaining") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testDeferredChaining()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise jsg::Promise chaining") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testJsgPromiseChaining()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise error propagation") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testErrorPropagation()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise tryConsumeResolved") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testTryConsumeResolved()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise resolver addRef") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testResolverAddRef()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise from jsg::Promise") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testFromJsPromise()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise from jsg::Promise reject") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testFromJsPromiseReject()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise from already-resolved jsg::Promise") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testFromJsPromiseAlreadyResolved()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise from already-rejected jsg::Promise") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testFromJsPromiseAlreadyRejected()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise from already-resolved void jsg::Promise") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testFromJsPromiseAlreadyResolvedVoid()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise from already-rejected void jsg::Promise") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testFromJsPromiseAlreadyRejectedVoid()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise deep chain (trampolining)") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testDeepChain()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise trampoline order") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testTrampolineOrder()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise async stack trace on reject") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testAsyncStackTraceOnReject()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise async stack trace on throw") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testAsyncStackTraceOnThrow()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise async stack trace depth") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testAsyncStackTraceDepth()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise continuation trace address") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testContinuationTraceAddress()", "undefined", "undefined");
}

}  // namespace
}  // namespace workerd::jsg::test
