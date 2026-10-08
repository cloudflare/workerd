// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bench.h"

#include <workerd/io/io-context.h>
#include <workerd/rust/histogram/ffi.rs.h>
#include <workerd/util/thread-scopes.h>

#include <kj/glob-filter.h>

#include <cmath>

#if _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace workerd::api {

namespace {

// Defaults for options that neither the case nor the command line sets.
constexpr uint64_t DEFAULT_MIN_TIME_NS = 1'000'000'000;
constexpr uint64_t DEFAULT_WARMUP_NS = 200'000'000;
constexpr uint32_t DEFAULT_SAMPLES = 50;

// Under --quick, budgets are capped at these.
constexpr uint64_t QUICK_MIN_TIME_NS = 10'000'000;
constexpr uint64_t QUICK_WARMUP_NS = 2'000'000;
constexpr uint32_t QUICK_SAMPLES = 5;

// A calibrated batch takes at least this long, so that clock resolution and the cost of reading
// the clocks don't matter.
constexpr uint64_t MIN_BATCH_NS = 1'000'000;

constexpr uint64_t MAX_CALLS_PER_BATCH = uint64_t{1} << 40;
constexpr uint32_t MAX_SAMPLES = 1'000'000;

// Samples are recorded into histograms in picoseconds, up to an hour per call.
constexpr int64_t HISTOGRAM_HIGHEST_PS = 3'600'000'000'000'000;
constexpr int32_t HISTOGRAM_FIGURES = 3;
constexpr double CONFIDENCE = 0.95;

// A case is flagged as high-variance when the coefficient of variation of its wall time per call
// exceeds this.
constexpr double HIGH_VARIANCE_CV = 0.1;

int64_t monotonicNs() {
  return (kj::systemPreciseMonotonicClock().now() - kj::origin<kj::TimePoint>()) / kj::NANOSECONDS;
}

int64_t threadCpuNs() {
#if _WIN32
  // GetThreadTimes() only advances on scheduler ticks (about 15ms), so with 1ms batches the CPU
  // time samples on Windows are mostly noise. Windows has no precise thread CPU clock.
  FILETIME creation, exit, kernel, user;
  KJ_ASSERT(GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user));
  auto ticks = [](const FILETIME& t) {
    return (static_cast<int64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  };
  // FILETIME counts 100 ns ticks.
  return (ticks(kernel) + ticks(user)) * 100;
#else
  struct timespec ts;
  KJ_SYSCALL(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts));
  return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
}

uint64_t durationToNs(double ns, kj::StringPtr what) {
  JSG_REQUIRE(std::isfinite(ns) && ns >= 0 && ns < 1e18, RangeError, what,
      " must be a non-negative duration.");
  return static_cast<uint64_t>(ns);
}

uint64_t parseDuration(const BenchDuration& duration, kj::StringPtr what) {
  KJ_SWITCH_ONEOF(duration) {
    KJ_CASE_ONEOF(ms, double) {
      return durationToNs(ms * 1e6, what);
    }
    KJ_CASE_ONEOF(text, kj::String) {
      size_t numberEnd = 0;
      while (numberEnd < text.size() &&
          (('0' <= text[numberEnd] && text[numberEnd] <= '9') || text[numberEnd] == '.')) {
        ++numberEnd;
      }
      auto number =
          JSG_REQUIRE_NONNULL(kj::str(text.first(numberEnd)).tryParseAs<double>(), TypeError, what,
              " must be a number of milliseconds or a string like \"500ms\" or \"2s\".");
      auto unit = text.slice(numberEnd);
      double scale;
      if (unit == "ns"_kj) {
        scale = 1;
      } else if (unit == "us"_kj || unit == "µs"_kj) {
        scale = 1e3;
      } else if (unit == "ms"_kj || unit == ""_kj) {
        scale = 1e6;
      } else if (unit == "s"_kj) {
        scale = 1e9;
      } else {
        JSG_FAIL_REQUIRE(TypeError, what, " has an unknown unit \"", unit,
            "\"; use \"ns\", \"us\", \"ms\", or \"s\".");
      }
      return durationToNs(number * scale, what);
    }
  }
  KJ_UNREACHABLE;
}

uint64_t parseCount(double value, uint64_t max, kj::StringPtr what) {
  JSG_REQUIRE(value >= 1 && value <= static_cast<double>(max) && value == std::floor(value),
      RangeError, what, " must be an integer from 1 to ", max, ".");
  return static_cast<uint64_t>(value);
}

// A promise for `value`, awaiting it if it is a promise.
jsg::Promise<jsg::Value> settle(jsg::Lock& js, jsg::Value value) {
  if (value.getHandle(js)->IsPromise()) {
    return js.toPromise(value.getHandle(js));
  }
  return js.resolvedPromise(kj::mv(value));
}

::workerd::rust::histogram::Summary fillMetric(
    kj::ArrayPtr<const double> samples, bench::BenchReport::Metric::Builder metric) {
  auto list = metric.initSamples(samples.size());
  for (auto i: kj::indices(samples)) {
    list.set(i, samples[i]);
  }

  auto histogram =
      ::workerd::rust::histogram::new_histogram(1, HISTOGRAM_HIGHEST_PS, HISTOGRAM_FIGURES);
  for (auto sample: samples) {
    // Values over the highest trackable value are counted as exceeding and left out.
    histogram->record(static_cast<int64_t>(std::llround(kj::max(sample, 0.0) * 1000)));
  }
  auto summary = ::workerd::rust::histogram::summarize(*histogram, CONFIDENCE);

  auto ns = [](auto ps) { return static_cast<double>(ps) / 1000; };
  metric.setMean(ns(summary.mean));
  metric.setMeanLow(ns(summary.mean_lower));
  metric.setMeanHigh(ns(summary.mean_upper));
  metric.setMedian(ns(summary.median));
  metric.setMedianLow(ns(summary.median_lower));
  metric.setMedianHigh(ns(summary.median_upper));
  metric.setP75(ns(summary.p75));
  metric.setP99(ns(summary.p99));
  metric.setMin(ns(summary.min));
  metric.setMax(ns(summary.max));
  metric.setStddev(ns(summary.stddev));
  metric.setSkewness(summary.skewness);
  metric.setKurtosis(summary.kurtosis);
  return summary;
}

}  // namespace

kj::String describeBenchException(jsg::Lock& js, const jsg::Value& exception) {
  return js.tryCatch([&]() -> kj::String {
    auto value = jsg::JsValue(exception.getHandle(js));
    KJ_IF_SOME(object, value.tryCast<jsg::JsObject>()) {
      auto stack = object.get(js, "stack");
      if (stack.isString()) {
        return stack.toString(js);
      }
    }
    return value.toString(js);
  }, [](jsg::Value&&) { return kj::str("(an exception that can't be converted to a string)"); });
}

void fillBenchReport(const BenchGroupResult& result, bench::BenchReport::Group::Builder report) {
  KJ_IF_SOME(error, result.error) {
    report.setError(error);
  }
  auto cases = report.initCases(result.cases.size());
  for (auto i: kj::indices(result.cases)) {
    auto& from = result.cases[i];
    auto to = cases[i];
    to.setName(from.name);
    to.setStatus(from.status);
    KJ_IF_SOME(error, from.error) {
      to.setError(error);
    }
    if (from.status != bench::BenchReport::Case::Status::OK) {
      continue;
    }
    to.setIterationsPerSample(from.iterationsPerSample);
    auto wall = fillMetric(from.wallNs, to.initWallNs());
    fillMetric(from.cpuNs, to.initCpuNs());
    if (wall.mean > 0 && wall.stddev / wall.mean > HIGH_VARIANCE_CV) {
      to.initFlags(1).set(0, bench::BenchReport::Case::Flag::HIGH_VARIANCE);
    }
  }
}

struct BenchController::BatchTiming {
  int64_t wallNs = 0;
  int64_t cpuNs = 0;

  static BatchTiming now() {
    return {.wallNs = monotonicNs(), .cpuNs = threadCpuNs()};
  }
  BatchTiming operator-(const BatchTiming& other) const {
    return {.wallNs = wallNs - other.wallNs, .cpuNs = cpuNs - other.cpuNs};
  }
};

struct BenchController::CaseRun {
  enum class Phase { WARMUP, CALIBRATE, SAMPLE };

  size_t index;
  Phase phase;
  uint64_t calls = 1;
  uint64_t warmedUpNs = 0;
  // Calibration grows batches until one takes at least this long.
  uint64_t targetBatchNs;
  kj::Vector<double> wallNs;
  kj::Vector<double> cpuNs;
};

BenchController::BenchController(bench::BenchParams::Reader params)
    : caseFilter(kj::str(params.getCaseFilter())),
      quick(params.getQuick()) {
  // Only `workerd bench` delivers the bench event; fail closed anywhere else.
  KJ_REQUIRE(isBenchMode(), "the bench event is only supported by `workerd bench`");

  auto defaults = params.getDefaults();
  defaultMinTimeNs = defaults.getMinTimeNs() > 0 ? defaults.getMinTimeNs() : DEFAULT_MIN_TIME_NS;
  defaultWarmupNs = defaults.getWarmupNs() > 0 ? defaults.getWarmupNs() : DEFAULT_WARMUP_NS;
  defaultSamples = defaults.getSamples() > 0 ? defaults.getSamples() : DEFAULT_SAMPLES;
  defaultBatch = defaults.getBatch();
}

void BenchController::run(jsg::Lock& js,
    kj::String name,
    v8::Local<v8::Function> fn,
    jsg::Optional<BenchCaseOptions> maybeOptions) {
  JSG_REQUIRE(!running, Error,
      "BenchController.run() must be called by bench(), before it returns or its promise "
      "resolves.");
  for (auto& c: cases) {
    JSG_REQUIRE(c.name != name, Error, "A case named \"", name, "\" is already registered.");
  }

  auto options = kj::mv(maybeOptions).orDefault({});
  Case c{
    .name = kj::mv(name),
    .fn = js.v8Ref(fn),
    .setup = kj::mv(options.setup),
    .teardown = kj::mv(options.teardown),
    .skip = options.skip.orDefault(false),
    .minTimeNs = defaultMinTimeNs,
    .warmupNs = defaultWarmupNs,
    .samples = defaultSamples,
    .batch = defaultBatch,
  };
  KJ_IF_SOME(minTime, options.minTime) {
    c.minTimeNs = parseDuration(minTime, "minTime");
  }
  KJ_IF_SOME(warmup, options.warmup) {
    c.warmupNs = parseDuration(warmup, "warmup");
  }
  KJ_IF_SOME(samples, options.samples) {
    c.samples = static_cast<uint32_t>(parseCount(samples, MAX_SAMPLES, "samples"));
  }
  KJ_IF_SOME(batch, options.batch) {
    c.batch = parseCount(batch, MAX_CALLS_PER_BATCH, "batch");
  }
  if (quick) {
    c.minTimeNs = kj::min(c.minTimeNs, QUICK_MIN_TIME_NS);
    c.warmupNs = kj::min(c.warmupNs, QUICK_WARMUP_NS);
    c.samples = kj::min(c.samples, QUICK_SAMPLES);
  }
  cases.add(kj::mv(c));
}

BenchController::Options BenchController::getOptions() {
  return {
    .filter =
        caseFilter == ""_kj || caseFilter == "*"_kj ? kj::none : kj::Maybe(kj::str(caseFilter)),
    .quick = quick,
  };
}

jsg::Promise<BenchGroupResult> BenchController::runCases(jsg::Lock& js) {
  running = true;
  if (cases.empty()) {
    return js.resolvedPromise(BenchGroupResult{
      .error = kj::str("bench() did not register any cases."),
    });
  }
  return runCasesFrom(js, 0, {});
}

jsg::Promise<BenchGroupResult> BenchController::runCasesFrom(
    jsg::Lock& js, size_t index, kj::Vector<BenchCaseResult> results) {
  kj::GlobFilter filter(caseFilter == ""_kj ? "*"_kj : caseFilter.asPtr());
  while (index < cases.size() && !filter.matches(cases[index].name)) {
    ++index;
  }
  if (index == cases.size()) {
    return js.resolvedPromise(BenchGroupResult{.cases = results.releaseAsArray()});
  }

  return runCase(js, index).then(js,
      IoContext::current().addFunctor([self = JSG_THIS, index, results = kj::mv(results)](
                                          jsg::Lock& js, BenchCaseResult result) mutable {
    results.add(kj::mv(result));
    return self->runCasesFrom(js, index + 1, kj::mv(results));
  }));
}

jsg::Promise<BenchCaseResult> BenchController::runCase(jsg::Lock& js, size_t index) {
  auto& c = cases[index];
  if (c.skip) {
    return js.resolvedPromise(BenchCaseResult{
      .name = kj::str(c.name),
      .status = bench::BenchReport::Case::Status::SKIPPED,
    });
  }

  using Phase = CaseRun::Phase;
  auto run = kj::heap<CaseRun>(CaseRun{
    .index = index,
    .phase = c.warmupNs > 0 ? Phase::WARMUP
        : c.batch > 0       ? Phase::SAMPLE
                            : Phase::CALIBRATE,
    .calls = c.batch > 0 && c.warmupNs == 0 ? c.batch : 1,
    .targetBatchNs = kj::max(c.minTimeNs / c.samples, MIN_BATCH_NS),
  });

  // Starting from a continuation turns exceptions thrown by the first batch into a rejection.
  return js.resolvedPromise()
      .then(js,
          IoContext::current().addFunctor(
              [self = JSG_THIS, run = kj::mv(run)](jsg::Lock& js) mutable {
    return self->step(js, kj::mv(run));
  })).catch_(js, [name = kj::str(c.name)](jsg::Lock& js, jsg::Value exception) mutable {
    return BenchCaseResult{
      .name = kj::mv(name),
      .status = bench::BenchReport::Case::Status::FAILED,
      .error = describeBenchException(js, exception),
    };
  });
}

jsg::Promise<BenchCaseResult> BenchController::step(jsg::Lock& js, kj::Own<CaseRun> run) {
  // Yield to the event loop before each batch, so that pending I/O and timers make progress
  // outside the timed region.
  auto& context = IoContext::current();
  return context.awaitIo(
      js, kj::evalLater([] {}), [self = JSG_THIS, run = kj::mv(run)](jsg::Lock& js) mutable {
    auto index = run->index;
    auto calls = run->calls;
    return self->runBatch(js, index, calls)
        .then(js,
            IoContext::current().addFunctor(
                [self = self.addRef(), run = kj::mv(run)](
                    jsg::Lock& js, BatchTiming timing) mutable -> jsg::Promise<BenchCaseResult> {
      using Phase = CaseRun::Phase;
      auto& c = self->cases[run->index];
      auto wallNs = static_cast<uint64_t>(kj::max(timing.wallNs, int64_t{0}));
      auto grow = [&]() {
        if (wallNs < run->targetBatchNs && run->calls < MAX_CALLS_PER_BATCH) {
          run->calls *= 2;
          return true;
        }
        return false;
      };

      switch (run->phase) {
        case Phase::WARMUP:
          run->warmedUpNs += wallNs;
          if (run->warmedUpNs < c.warmupNs) {
            grow();
          } else if (c.batch > 0) {
            run->phase = Phase::SAMPLE;
            run->calls = c.batch;
          } else {
            run->phase = Phase::CALIBRATE;
          }
          break;
        case Phase::CALIBRATE:
          if (!grow()) {
            run->phase = Phase::SAMPLE;
          }
          break;
        case Phase::SAMPLE: {
          auto calls = static_cast<double>(run->calls);
          run->wallNs.add(static_cast<double>(timing.wallNs) / calls);
          run->cpuNs.add(static_cast<double>(timing.cpuNs) / calls);
          if (run->wallNs.size() == c.samples) {
            return js.resolvedPromise(BenchCaseResult{
              .name = kj::str(c.name),
              .iterationsPerSample = run->calls,
              .wallNs = run->wallNs.releaseAsArray(),
              .cpuNs = run->cpuNs.releaseAsArray(),
            });
          }
          break;
        }
      }
      return self->step(js, kj::mv(run));
    }));
  });
}

jsg::Promise<BenchController::BatchTiming> BenchController::runBatch(
    jsg::Lock& js, size_t index, uint64_t calls) {
  // Times the batch given the setup result (undefined without `setup`), then runs `teardown`.
  auto timeBatch = [self = JSG_THIS, index, calls](
                       jsg::Lock& js, jsg::Value state) mutable -> jsg::Promise<BatchTiming> {
    if (self->cases[index].teardown == kj::none) {
      return self->callBatch(js, index, kj::mv(state), calls, BatchTiming::now());
    }
    auto teardownState = state.addRef(js);
    return self->callBatch(js, index, kj::mv(state), calls, BatchTiming::now())
        .then(js,
            IoContext::current().addFunctor(
                [self = self.addRef(), index, state = kj::mv(teardownState)](
                    jsg::Lock& js, BatchTiming timing) mutable {
      auto& teardown = KJ_ASSERT_NONNULL(self->cases[index].teardown);
      return settle(js, teardown(js, kj::mv(state))).then(js, [timing](jsg::Lock&, jsg::Value) {
        return timing;
      });
    }));
  };

  KJ_IF_SOME(setup, cases[index].setup) {
    return settle(js, setup(js)).then(js, IoContext::current().addFunctor(kj::mv(timeBatch)));
  }
  return timeBatch(js, js.v8Ref(js.v8Undefined()));
}

jsg::Promise<BenchController::BatchTiming> BenchController::callBatch(
    jsg::Lock& js, size_t index, jsg::Value state, uint64_t remaining, BatchTiming start) {
  auto& c = cases[index];
  auto isolate = js.v8Isolate;
  auto context = js.v8Context();
  auto fn = c.fn.getHandle(js);
  auto receiver = js.v8Undefined();
  v8::Local<v8::Value> argument = state.getHandle(js);
  int argumentCount = c.setup == kj::none ? 0 : 1;

  while (remaining > 0) {
    v8::HandleScope scope(isolate);
    --remaining;
    auto result = jsg::check(fn->Call(context, receiver, argumentCount, &argument));
    if (result->IsPromise()) {
      // Await the result before the next call; the batch's time includes the wait.
      return js.toPromise(result).then(js,
          IoContext::current().addFunctor([self = JSG_THIS, index, state = kj::mv(state), remaining,
                                              start](jsg::Lock& js, jsg::Value) mutable {
        return self->callBatch(js, index, kj::mv(state), remaining, start);
      }));
    }
  }
  return js.resolvedPromise(BatchTiming::now() - start);
}

void BenchController::visitForGc(jsg::GcVisitor& visitor) {
  for (auto& c: cases) {
    visitor.visit(c.fn, c.setup, c.teardown);
  }
}

}  // namespace workerd::api
