// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Benchmarks comparing jsg::Promise<T> vs jsg::DeferredPromise<T>
//
// Run with: bazel run //src/workerd/tests:bench-deferred-promise
//
// These benchmarks demonstrate the performance benefits of DeferredPromise
// in scenarios where promises often resolve synchronously:
//
// 1. Immediate resolution - DeferredPromise avoids V8 promise allocation
// 2. Synchronous continuation chains - All callbacks run immediately
// 3. Pending with continuations - Setup overhead comparison
// 4. Conversion to JS - Cost when you do need a V8 promise

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

namespace workerd {
namespace {

// =============================================================================
// Benchmark 1: Immediate Resolution
// =============================================================================
// Measures the cost of creating a promise that is immediately resolved.
// DeferredPromise should be significantly faster as it doesn't create V8 objects.

static void Promise_ImmediateResolve_JsgPromise(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        auto promise = js.resolvedPromise(42);
        benchmark::DoNotOptimize(promise);
      }
    }
  });
}
WD_BENCHMARK(Promise_ImmediateResolve_JsgPromise);

static void Promise_ImmediateResolve_Deferred(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        auto promise = jsg::DeferredPromise<int>::resolved(42);
        benchmark::DoNotOptimize(promise);
      }
    }
  });
}
WD_BENCHMARK(Promise_ImmediateResolve_Deferred);

// =============================================================================
// Benchmark 2: Single Continuation on Already-Resolved Promise
// =============================================================================
// Measures the overhead of attaching a .then() to an already-resolved promise.
// jsg::Promise runs via microtask queue; DeferredPromise runs synchronously.

static void Promise_ThenOnResolved_JsgPromise(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        auto promise = js.resolvedPromise(42);
        promise.then(js, [&result](jsg::Lock&, int value) { result = value * 2; });
        js.runMicrotasks();
        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_ThenOnResolved_JsgPromise);

static void Promise_ThenOnResolved_Deferred(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        auto promise = jsg::DeferredPromise<int>::resolved(42);
        promise.then(js, [&result](jsg::Lock&, int value) { result = value * 2; });
        // No microtasks needed - runs synchronously!
        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_ThenOnResolved_Deferred);

// =============================================================================
// Benchmark 3: Chain of Continuations on Already-Resolved Promise
// =============================================================================
// Measures chains like .then().then().then() on already-resolved promises.
// This is a common pattern in stream implementations.

static void Promise_ChainOnResolved_JsgPromise(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        js.resolvedPromise(1).then(js, [](jsg::Lock&, int v) {
          return v + 1;
        }).then(js, [](jsg::Lock&, int v) {
          return v * 2;
        }).then(js, [](jsg::Lock&, int v) {
          return v + 10;
        }).then(js, [&result](jsg::Lock&, int v) { result = v; });
        js.runMicrotasks();
        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_ChainOnResolved_JsgPromise);

static void Promise_ChainOnResolved_Deferred(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        jsg::DeferredPromise<int>::resolved(1).then(js, [](jsg::Lock&, int v) {
          return v + 1;
        }).then(js, [](jsg::Lock&, int v) {
          return v * 2;
        }).then(js, [](jsg::Lock&, int v) {
          return v + 10;
        }).then(js, [&result](jsg::Lock&, int v) { result = v; });
        // No microtasks - all 4 callbacks ran synchronously!
        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_ChainOnResolved_Deferred);

// =============================================================================
// Benchmark 4: Create Pending, Attach Continuation, Then Resolve
// =============================================================================
// Measures the full lifecycle: create pending promise, attach callback, resolve.
// This is the most common pattern for async operations.

static void Promise_PendingThenResolve_JsgPromise(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        auto [promise, resolver] = js.newPromiseAndResolver<int>();
        promise.then(js, [&result](jsg::Lock&, int value) { result = value; });
        resolver.resolve(js, 42);
        js.runMicrotasks();
        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_PendingThenResolve_JsgPromise);

static void Promise_PendingThenResolve_Deferred(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        auto [promise, resolver] = jsg::newDeferredPromiseAndResolver<int>();
        promise.then(js, [&result](jsg::Lock&, int value) { result = value; });
        resolver.resolve(js, 42);
        // Callback already ran!
        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_PendingThenResolve_Deferred);

// =============================================================================
// Benchmark 5: Chain on Pending Promise
// =============================================================================
// Measures setting up a chain of continuations before resolution.

static void Promise_ChainPendingThenResolve_JsgPromise(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        auto [promise, resolver] = js.newPromiseAndResolver<int>();
        kj::mv(promise).then(js, [](jsg::Lock&, int v) {
          return v + 1;
        }).then(js, [](jsg::Lock&, int v) {
          return v * 2;
        }).then(js, [](jsg::Lock&, int v) {
          return v + 10;
        }).then(js, [&result](jsg::Lock&, int v) { result = v; });
        resolver.resolve(js, 1);
        js.runMicrotasks();
        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_ChainPendingThenResolve_JsgPromise);

static void Promise_ChainPendingThenResolve_Deferred(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        auto [promise, resolver] = jsg::newDeferredPromiseAndResolver<int>();
        kj::mv(promise).then(js, [](jsg::Lock&, int v) {
          return v + 1;
        }).then(js, [](jsg::Lock&, int v) {
          return v * 2;
        }).then(js, [](jsg::Lock&, int v) {
          return v + 10;
        }).then(js, [&result](jsg::Lock&, int v) { result = v; });
        resolver.resolve(js, 1);
        // All 4 callbacks ran synchronously during resolve()!
        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_ChainPendingThenResolve_Deferred);

// =============================================================================
// Benchmark 6: Conversion to jsg::Promise
// =============================================================================
// Measures the cost of converting DeferredPromise to jsg::Promise.
// This is the "escape hatch" when you need to expose a promise to JS.

static void Promise_ToJsPromise_AlreadyResolved(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        auto deferred = jsg::DeferredPromise<int>::resolved(42);
        auto jsPromise = deferred.toJsPromise(js);
        benchmark::DoNotOptimize(jsPromise);
      }
    }
  });
}
WD_BENCHMARK(Promise_ToJsPromise_AlreadyResolved);

static void Promise_ToJsPromise_Pending(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        auto [promise, resolver] = jsg::newDeferredPromiseAndResolver<int>();
        auto jsPromise = promise.toJsPromise(js);
        resolver.resolve(js, 42);
        js.runMicrotasks();
        benchmark::DoNotOptimize(jsPromise);
      }
    }
  });
}
WD_BENCHMARK(Promise_ToJsPromise_Pending);

// =============================================================================
// Benchmark 7: fromJsPromise - Converting jsg::Promise to Deferred
// =============================================================================
// Measures the benefit of converting to DeferredPromise for internal processing.
// The continuation chain runs synchronously once the JS promise resolves.

static void Promise_FromJsPromise_WithChain(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        auto [jsPromise, jsResolver] = js.newPromiseAndResolver<int>();

        // Convert to DeferredPromise and set up chain
        auto deferred = jsg::DeferredPromise<int>::fromJsPromise(js, kj::mv(jsPromise));
        kj::mv(deferred).then(js, [](jsg::Lock&, int v) {
          return v + 1;
        }).then(js, [](jsg::Lock&, int v) {
          return v * 2;
        }).then(js, [](jsg::Lock&, int v) {
          return v + 10;
        }).then(js, [&result](jsg::Lock&, int v) { result = v; });

        jsResolver.resolve(js, 1);
        js.runMicrotasks();  // Only need microtasks for initial JS promise
        // All 4 deferred continuations ran synchronously after microtask!
        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_FromJsPromise_WithChain);

// Compare with pure jsg::Promise chain
static void Promise_PureJsPromise_Chain(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        auto [jsPromise, jsResolver] = js.newPromiseAndResolver<int>();

        kj::mv(jsPromise).then(js, [](jsg::Lock&, int v) {
          return v + 1;
        }).then(js, [](jsg::Lock&, int v) {
          return v * 2;
        }).then(js, [](jsg::Lock&, int v) {
          return v + 10;
        }).then(js, [&result](jsg::Lock&, int v) { result = v; });

        jsResolver.resolve(js, 1);
        js.runMicrotasks();  // Each .then() goes through microtask queue
        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_PureJsPromise_Chain);

// =============================================================================
// Benchmark 7b: fromJsPromise - Already Settled Optimization
// =============================================================================
// Measures the optimization when the JS promise is already resolved/rejected.
// This avoids the microtask queue entirely.

static void Promise_FromJsPromise_AlreadyResolved(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        // Create an already-resolved JS promise
        auto jsPromise = js.resolvedPromise(42);

        // Convert to DeferredPromise - should detect it's already resolved
        auto deferred = jsg::DeferredPromise<int>::fromJsPromise(js, kj::mv(jsPromise));

        // This should run immediately - no microtasks needed!
        kj::mv(deferred).then(js, [&result](jsg::Lock&, int v) { result = v * 2; });

        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_FromJsPromise_AlreadyResolved);

static void Promise_FromJsPromise_AlreadyRejected(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        // Create an already-rejected JS promise
        auto jsPromise = js.rejectedPromise<int>(JSG_KJ_EXCEPTION(FAILED, Error, "test error"));

        // Convert to DeferredPromise - should detect it's already rejected
        auto deferred = jsg::DeferredPromise<int>::fromJsPromise(js, kj::mv(jsPromise));

        // Error handler should run immediately - no microtasks needed!
        kj::mv(deferred).then(
            js, [](jsg::Lock&, int) { return 0; }, [&result](jsg::Lock&, kj::Exception) {
          result = -1;
          return -1;
        });

        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_FromJsPromise_AlreadyRejected);

// Compare: fromJsPromise with pending promise (requires microtasks)
static void Promise_FromJsPromise_Pending(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        auto [jsPromise, jsResolver] = js.newPromiseAndResolver<int>();

        // Convert to DeferredPromise while pending
        auto deferred = jsg::DeferredPromise<int>::fromJsPromise(js, kj::mv(jsPromise));

        kj::mv(deferred).then(js, [&result](jsg::Lock&, int v) { result = v * 2; });

        jsResolver.resolve(js, 42);
        js.runMicrotasks();  // Need microtasks for pending case

        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_FromJsPromise_Pending);

// Compare: Direct use of already-resolved JS promise (no conversion)
static void Promise_DirectJsPromise_AlreadyResolved(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        auto jsPromise = js.resolvedPromise(42);

        // Use JS promise directly - always needs microtasks
        kj::mv(jsPromise).then(js, [&result](jsg::Lock&, int v) { result = v * 2; });
        js.runMicrotasks();

        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_DirectJsPromise_AlreadyResolved);

// =============================================================================
// Benchmark 8: Void Promises
// =============================================================================
// Measures void promise performance (common for signaling completion).

static void Promise_Void_JsgPromise(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        bool done = false;
        auto [promise, resolver] = js.newPromiseAndResolver<void>();
        kj::mv(promise).then(js, [&done](jsg::Lock&) { done = true; });
        resolver.resolve(js);
        js.runMicrotasks();
        benchmark::DoNotOptimize(done);
      }
    }
  });
}
WD_BENCHMARK(Promise_Void_JsgPromise);

static void Promise_Void_Deferred(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        bool done = false;
        auto [promise, resolver] = jsg::newDeferredPromiseAndResolver<void>();
        kj::mv(promise).then(js, [&done](jsg::Lock&) { done = true; });
        resolver.resolve(js);
        benchmark::DoNotOptimize(done);
      }
    }
  });
}
WD_BENCHMARK(Promise_Void_Deferred);

// =============================================================================
// Benchmark 9: Error Handling with catch_()
// =============================================================================
// Measures error path performance.

static void Promise_Rejection_JsgPromise(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        auto [promise, resolver] = js.newPromiseAndResolver<int>();
        kj::mv(promise).then(
            js, [](jsg::Lock&, int v) { return v; }, [&result](jsg::Lock&, jsg::Value) {
          result = -1;
          return -1;
        });
        resolver.reject(js, jsg::v8Str(js.v8Isolate, "error"));
        js.runMicrotasks();
        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_Rejection_JsgPromise);

static void Promise_Rejection_Deferred(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        int result = 0;
        auto [promise, resolver] = jsg::newDeferredPromiseAndResolver<int>();
        kj::mv(promise).then(
            js, [](jsg::Lock&, int v) { return v; }, [&result](jsg::Lock&, kj::Exception) {
          result = -1;
          return -1;
        });
        resolver.reject(js, JSG_KJ_EXCEPTION(FAILED, Error, "error"));
        benchmark::DoNotOptimize(result);
      }
    }
  });
}
WD_BENCHMARK(Promise_Rejection_Deferred);

// =============================================================================
// Benchmark 10: Mixed Workload - Simulating Stream Read
// =============================================================================
// Simulates a realistic stream-like pattern where most reads are immediately
// available (from buffer) but some require waiting for I/O.

static void Promise_StreamSimulation_JsgPromise(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    // Simulate 90% immediate, 10% pending
    size_t totalBytes = 0;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        if (i % 10 == 0) {
          // Pending case (10%)
          auto [promise, resolver] = js.newPromiseAndResolver<size_t>();
          kj::mv(promise).then(
              js, [&totalBytes](jsg::Lock&, size_t bytes) { totalBytes += bytes; });
          resolver.resolve(js, size_t{64});
          js.runMicrotasks();
        } else {
          // Immediate case (90%)
          js.resolvedPromise(size_t{64}).then(js, [&totalBytes](jsg::Lock&, size_t bytes) {
            totalBytes += bytes;
          });
          js.runMicrotasks();
        }
      }
      benchmark::DoNotOptimize(totalBytes);
    }
  });
}
WD_BENCHMARK(Promise_StreamSimulation_JsgPromise);

static void Promise_StreamSimulation_Deferred(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    // Simulate 90% immediate, 10% pending
    size_t totalBytes = 0;

    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        if (i % 10 == 0) {
          // Pending case (10%)
          auto [promise, resolver] = jsg::newDeferredPromiseAndResolver<size_t>();
          kj::mv(promise).then(
              js, [&totalBytes](jsg::Lock&, size_t bytes) { totalBytes += bytes; });
          resolver.resolve(js, size_t{64});
          // No microtasks needed!
        } else {
          // Immediate case (90%)
          jsg::DeferredPromise<size_t>::resolved(64).then(
              js, [&totalBytes](jsg::Lock&, size_t bytes) { totalBytes += bytes; });
          // No microtasks needed!
        }
      }
      benchmark::DoNotOptimize(totalBytes);
    }
  });
}
WD_BENCHMARK(Promise_StreamSimulation_Deferred);

// =============================================================================
// Benchmark 11: tryConsumeResolved() Fast Path
// =============================================================================
// Measures the optimization of checking if a promise is already resolved
// without consuming it through the normal .then() path.

static void Promise_TryConsumeResolved(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        auto promise = jsg::DeferredPromise<int>::resolved(42);
        auto value = promise.tryConsumeResolved();
        benchmark::DoNotOptimize(value);
      }
    }
  });
}
WD_BENCHMARK(Promise_TryConsumeResolved);

// =============================================================================
// Benchmark 12: Deep Chain (Trampolining)
// =============================================================================
// Tests that deep chains work without stack overflow thanks to trampolining.
// Also measures the overhead of trampolining for very deep chains.

static void Promise_DeepChain_Deferred(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      // Build a chain of 100 .then() calls
      constexpr size_t CHAIN_DEPTH = 100;
      int result = 0;

      auto [promise, resolver] = jsg::newDeferredPromiseAndResolver<int>();
      auto current = kj::mv(promise);

      for (size_t i = 0; i < CHAIN_DEPTH; ++i) {
        current = kj::mv(current).then(js, [](jsg::Lock&, int v) { return v + 1; });
      }

      current.then(js, [&result](jsg::Lock&, int v) { result = v; });

      resolver.resolve(js, 0);
      benchmark::DoNotOptimize(result);
    }
  });
}
WD_BENCHMARK(Promise_DeepChain_Deferred);

static void Promise_DeepChain_JsgPromise(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    for (auto _: state) {
      // Build a chain of 100 .then() calls
      constexpr size_t CHAIN_DEPTH = 100;
      int result = 0;

      auto [promise, resolver] = js.newPromiseAndResolver<int>();
      auto current = kj::mv(promise);

      for (size_t i = 0; i < CHAIN_DEPTH; ++i) {
        current = kj::mv(current).then(js, [](jsg::Lock&, int v) { return v + 1; });
      }

      current.then(js, [&result](jsg::Lock&, int v) { result = v; });

      resolver.resolve(js, 0);
      js.runMicrotasks();
      benchmark::DoNotOptimize(result);
    }
  });
}
WD_BENCHMARK(Promise_DeepChain_JsgPromise);

}  // namespace
}  // namespace workerd
