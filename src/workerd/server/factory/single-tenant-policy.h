// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The policy objects of a single-tenant runtime: no limits, and the clock JavaScript sees. The
// worker factory gives them to every worker it compiles; `workerd repl` gives them to the isolate
// it evaluates input in. The production runtime supplies its own.

#include <workerd/io/actor-cache.h>
#include <workerd/io/io-channels.h>
#include <workerd/io/limit-enforcer.h>
#include <workerd/io/tracked-wasm-instance.h>

#include <kj/refcount.h>
#include <kj/time.h>
#include <kj/timer.h>

namespace workerd::server {

class NullIsolateLimitEnforcer final: public IsolateLimitEnforcer {
 public:
  v8::Isolate::CreateParams getCreateParams() override {
    return {};
  }
  void customizeIsolate(v8::Isolate* isolate) override {}
  ActorCacheSharedLruOptions getActorCacheLruOptions() override {
    return {.softLimit = 16 * (1ull << 20),
      .hardLimit = 128 * (1ull << 20),
      .staleTimeout = 30 * kj::SECONDS,
      .dirtyListByteLimit = 8 * (1ull << 20),
      .maxKeysPerRpc = 128,
      // In-memory-only actors: the cache never flushes to its (empty) backing storage.
      .neverFlush = true};
  }
  kj::Own<void> enterStartupJs(jsg::Lock&, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  kj::Own<void> enterStartupPython(
      jsg::Lock&, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  kj::Own<void> enterDynamicImportJs(
      jsg::Lock&, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  kj::Own<void> enterLoggingJs(jsg::Lock&, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  kj::Own<void> enterInspectorJs(
      jsg::Lock&, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  void completedRequest(kj::StringPtr id) const override {}
  bool exitJs(jsg::Lock& lock) const override {
    return false;
  }
  void reportMetrics(IsolateObserver& isolateMetrics) const override {}
  kj::Maybe<size_t> checkPbkdfIterations(jsg::Lock& lock, size_t iterations) const override {
    return kj::none;
  }
  bool hasExcessivelyExceededHeapLimit() const override {
    return false;
  }
  const TrackedWasmInstanceList& getTrackedWasmInstances() const override {
    return trackedWasmInstances;
  }

 private:
  TrackedWasmInstanceList trackedWasmInstances;
};

class NullLimitEnforcer final: public LimitEnforcer, public kj::Refcounted {
 public:
  kj::Own<void> enterJs(jsg::Lock& lock, IoContext& context) override {
    return {};
  }
  void topUpActor() override {}
  void newSubrequest(bool isInHouse) override {}
  void newKvRequest(KvOpType op) override {}
  void newAnalyticsEngineRequest() override {}
  kj::Promise<void> limitDrain() override {
    return kj::NEVER_DONE;
  }
  kj::Promise<void> limitScheduled() override {
    return kj::NEVER_DONE;
  }
  kj::Duration getAlarmLimit() override {
    return 15 * kj::MINUTES;
  }
  size_t getBufferingLimit() override {
    return kj::maxValue;
  }
  kj::Maybe<EventOutcome> getLimitsExceeded() override {
    return kj::none;
  }
  kj::Promise<void> onLimitsExceeded() override {
    return kj::NEVER_DONE;
  }
  void setCpuLimitNearlyExceededCallback(kj::Function<void()> cb) override {}
  void requireLimitsNotExceeded() override {}
  void reportMetrics(RequestObserver& requestMetrics) override {}
  kj::Duration consumeTimeElapsedForPeriodicLogging() override {
    return 0 * kj::SECONDS;
  }
  size_t getSqliteMemoryUsage() const override {
    return 0;
  }
};

// The clock JavaScript sees: the calendar clock for `now()`, timers measured from a fresh
// monotonic reading so that time spent in JavaScript since the last poll does not shorten them.
class WorkerTimerChannel final: public TimerChannel {
 public:
  WorkerTimerChannel(kj::Timer& timer, const kj::MonotonicClock& monotonicClock)
      : timer(timer),
        monotonicClock(monotonicClock) {}

  void syncTime() override {}
  kj::Date now(kj::Maybe<kj::Date>) override {
    return kj::systemPreciseCalendarClock().now();
  }
  kj::Promise<void> atTime(kj::Date when) override {
    return timer.atTime(monotonicClock.now() + (when - now(kj::none)));
  }
  kj::Promise<void> afterLimitTimeout(kj::Duration t) override {
    return timer.afterDelay(t);
  }
  kj::TimePoint nowForLimitTimeout() override {
    return monotonicClock.now();
  }

 private:
  kj::Timer& timer;
  const kj::MonotonicClock& monotonicClock;
};

}  // namespace workerd::server
