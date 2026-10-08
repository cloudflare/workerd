// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The `bench` handler: `BenchController`, which a Worker's `bench(b, env, ctx)` handler uses to
// register cases, and the runner that times them. Only `workerd bench` dispatches this event.
//
// The runtime runs the timing loop: user code supplies the function to time and never sees a
// high-resolution clock. For each case, the runner calls the function repeatedly to warm up,
// calibrates a batch size, then times `samples` batches, reading the monotonic and thread CPU
// clocks only between batches. Each sample is a batch's time divided by its number of calls.

#include <workerd/io/bench.capnp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

// Results of one case, before they are written to a `BenchReport::Case`.
struct BenchCaseResult {
  kj::String name;
  bench::BenchReport::Case::Status status = bench::BenchReport::Case::Status::OK;
  kj::Maybe<kj::String> error;
  uint64_t iterationsPerSample = 0;
  // Wall and thread CPU time per call, one value per sampled batch.
  kj::Array<double> wallNs;
  kj::Array<double> cpuNs;
};

// Results of one `bench()` handler.
struct BenchGroupResult {
  // Set if `bench()` itself failed.
  kj::Maybe<kj::String> error;
  kj::Array<BenchCaseResult> cases;
};

// The exception's stack if it has one, or else its string conversion.
kj::String describeBenchException(jsg::Lock& js, const jsg::Value& exception);

// Writes `result` to `report`, computing summary statistics of the samples. Doesn't set the
// group's name, which the caller knows.
void fillBenchReport(const BenchGroupResult& result, bench::BenchReport::Group::Builder report);

// A duration: a number of milliseconds, or a string with a unit, as in "500ms" or "2s".
using BenchDuration = kj::OneOf<double, kj::String>;

// Options for one case, the third argument of `BenchController.run()`.
struct BenchCaseOptions {
  // Sampling budget: the batch size is calibrated so that all samples take about this long.
  jsg::Optional<BenchDuration> minTime;
  // How long to call the function before calibrating and sampling, to let V8 optimize it.
  jsg::Optional<BenchDuration> warmup;
  // The number of batches to time.
  jsg::Optional<double> samples;
  // Calls per batch, instead of calibrating.
  jsg::Optional<double> batch;
  // Called before each batch, untimed. Its result (awaited if it is a promise) is passed to every
  // call of the function in that batch.
  jsg::Optional<jsg::Function<jsg::Value()>> setup;
  // Called after each batch with the setup result (undefined without `setup`), untimed, and
  // awaited if it returns a promise.
  jsg::Optional<jsg::Function<jsg::Value(jsg::Value)>> teardown;
  // Reports the case as skipped instead of running it.
  jsg::Optional<bool> skip;

  JSG_STRUCT(minTime, warmup, samples, batch, setup, teardown, skip);
};

// The first argument of a `bench()` handler.
//
// The bench types aren't in the generated TypeScript definitions: only `workerd bench` delivers
// the bench event, so they aren't part of the API that deployed Workers see. Nothing that the
// type generator visits refers to them.
class BenchController final: public jsg::Object {
 public:
  explicit BenchController(bench::BenchParams::Reader params);

  struct Options {
    // The case filter given on the command line, if any.
    jsg::Optional<kj::String> filter;
    // Whether `--quick` was given: budgets are short, so results only show that cases run.
    bool quick;

    JSG_STRUCT(filter, quick);
  };

  // Registers a case. Cases run, in registration order, after `bench()` returns or its promise
  // resolves. `fn` may return a promise, which is awaited before the next call.
  void run(jsg::Lock& js,
      kj::String name,
      v8::Local<v8::Function> fn,
      jsg::Optional<BenchCaseOptions> options);

  // Returns `value`. Passing a result through it keeps V8 from optimizing away the code that
  // computed it, since V8 can't see into the native call.
  jsg::JsValue blackBox(jsg::JsValue value) {
    return value;
  }

  Options getOptions();

  JSG_RESOURCE_TYPE(BenchController) {
    JSG_METHOD(run);
    JSG_METHOD(blackBox);
    JSG_READONLY_PROTOTYPE_PROPERTY(options, getOptions);
  }

  // Runs the registered cases, after the handler has registered them. Called from C++.
  jsg::Promise<BenchGroupResult> runCases(jsg::Lock& js);

 private:
  struct Case {
    kj::String name;
    jsg::V8Ref<v8::Function> fn;
    kj::Maybe<jsg::Function<jsg::Value()>> setup;
    kj::Maybe<jsg::Function<jsg::Value(jsg::Value)>> teardown;
    bool skip;
    uint64_t minTimeNs;
    uint64_t warmupNs;
    uint32_t samples;
    // 0 calibrates.
    uint64_t batch;
  };

  struct CaseRun;
  struct BatchTiming;

  kj::String caseFilter;
  bool quick;
  uint64_t defaultMinTimeNs;
  uint64_t defaultWarmupNs;
  uint32_t defaultSamples;
  uint64_t defaultBatch;

  kj::Vector<Case> cases;
  // Set once the cases start running; registering a case after that is an error.
  bool running = false;

  jsg::Promise<BenchGroupResult> runCasesFrom(
      jsg::Lock& js, size_t index, kj::Vector<BenchCaseResult> results);
  jsg::Promise<BenchCaseResult> runCase(jsg::Lock& js, size_t index);
  jsg::Promise<BenchCaseResult> step(jsg::Lock& js, kj::Own<CaseRun> run);
  jsg::Promise<BatchTiming> runBatch(jsg::Lock& js, size_t index, uint64_t calls);
  jsg::Promise<BatchTiming> callBatch(
      jsg::Lock& js, size_t index, jsg::Value state, uint64_t remaining, BatchTiming start);

  void visitForGc(jsg::GcVisitor& visitor);
};

#define EW_BENCH_ISOLATE_TYPES                                                                     \
  api::BenchController, api::BenchController::Options, api::BenchCaseOptions

}  // namespace workerd::api
