// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>
#include <kj/timer.h>

namespace workerd::test {

enum class TimerClock {
  // Applies the deadline clamp used by embedders with Spectre mitigations.
  CLAMPED,
  // Calendar reads ignore the clamp and stay fixed while the monotonic timer advances.
  // This exercises a channel whose calendar clock moves independently of its timer.
  UNCLAMPED,
};

class TestTimerChannel final: public TimerChannel {
 public:
  explicit TestTimerChannel(TimerClock clock): clock(clock) {}

  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date> clockClamp) override {
    if (clock == TimerClock::UNCLAMPED) return kj::UNIX_EPOCH;
    auto time = kj::UNIX_EPOCH + (timer.now() - kj::origin<kj::TimePoint>());
    return clockClamp.map([&](kj::Date deadline) {
      return kj::min(time, deadline);
    }).orDefault(time);
  }

  kj::Promise<void> atTime(kj::Date when) override {
    if (failNext) {
      failNext = false;
      KJ_FAIL_REQUIRE("injected timer failure");
    }
    auto delay = when - now(kj::none);
    if (delay <= 0 * kj::MILLISECONDS) return kj::READY_NOW;
    return timer.afterDelay(delay);
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration) override {
    return kj::NEVER_DONE;
  }

  void advanceTo(uint milliseconds) {
    timer.advanceTo(kj::origin<kj::TimePoint>() + milliseconds * kj::MILLISECONDS);
  }

  void failNextWait() {
    failNext = true;
  }

 private:
  TimerClock clock;
  kj::TimerImpl timer{kj::origin<kj::TimePoint>()};
  bool failNext = false;
};

// Fetch completion is controlled separately from the clock, so timer callbacks and independent
// I/O can be observed in a deterministic order.
class DelayedHttpResponse final: public WorkerInterface {
 public:
  explicit DelayedHttpResponse(kj::Promise<void> ready): ready(kj::mv(ready)) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    co_await ready;
    auto responseHeaders = headers.cloneShallow();
    responseHeaders.clear();
    response.send(200, "OK"_kj, responseHeaders, static_cast<uint64_t>(0));
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    KJ_UNIMPLEMENTED();
  }
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_UNIMPLEMENTED();
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_UNIMPLEMENTED();
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_UNIMPLEMENTED();
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }

 private:
  kj::Promise<void> ready;
};

class TimerTestIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
 public:
  TimerTestIoChannelFactory(TimerChannel& timer, kj::Promise<void> fetchReady)
      : DummyIoChannelFactory(timer),
        fetchReady(fetchReady.fork()) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    return kj::heap<DelayedHttpResponse>(fetchReady.addBranch());
  }

 private:
  kj::ForkedPromise<void> fetchReady;
};

// TestFixture uses V8's default Date clock. Scripts read performance.now(), which uses
// IoContext::now() directly, the same clock that the server's V8 platform exposes as Date.now().
class TimerTest {
 public:
  explicit TimerTest(kj::StringPtr source, TimerClock clock = TimerClock::CLAMPED)
      : timer(clock),
        fixture({.mainModuleSource = source,
          .actorId = Worker::Actor::Id(kj::str("timer-test")),
          .actorClassName = "default"_kj,
          .useRealTimers = false,
          .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
              [this](TimerChannel&) -> kj::Rc<IoChannelFactory> {
                return kj::rc<TimerTestIoChannelFactory>(timer, kj::mv(fetchReady.promise));
              })}),
        entrypoint(fixture.makeWorkerEntrypoint()),
        headers(headerTable),
        request(
            entrypoint->request(kj::HttpMethod::GET, "https://example.com", headers, body, response)
                .eagerlyEvaluate(nullptr)) {
    fixture.pollEventLoop();
  }

  void advanceTo(uint milliseconds) {
    advanceWithoutPolling(milliseconds);
    fixture.pollEventLoop();
  }

  void advanceWithoutPolling(uint milliseconds) {
    timer.advanceTo(milliseconds);
  }

  void failNextWait() {
    timer.failNextWait();
  }

  void expectTimeOutsideContext(uint milliseconds) {
    auto& context = KJ_ASSERT_NONNULL(fixture.getActor().getIoContext());
    KJ_ASSERT(!context.isCurrent());
    KJ_EXPECT(context.now() == kj::UNIX_EPOCH + milliseconds * kj::MILLISECONDS);
  }

  void completeFetch() {
    fetchReady.fulfiller->fulfill();
    fixture.pollEventLoop();
  }

  void expectEvents(kj::StringPtr expected) {
    fixture.enterWorkerLock([&](Worker::Lock& lock) {
      jsg::Lock& js = lock;
      js.withinHandleScope([&]() {
        v8::Context::Scope contextScope(lock.getContext());
        auto events = js.global().get(js, "events"_kj).toString(js);
        KJ_EXPECT(events == expected, events, expected);
      });
    });
  }

  void expectDone() {
    KJ_ASSERT(request.poll(fixture.getWaitScope()), "timer request did not complete");
    request.wait(fixture.getWaitScope());
    KJ_EXPECT(response.statusCode == 200);
  }

  void expectFailed(kj::StringPtr message) {
    KJ_ASSERT(request.poll(fixture.getWaitScope()), "timer request did not fail");
    KJ_EXPECT_THROW_MESSAGE(message, request.wait(fixture.getWaitScope()));
    fixture.pollEventLoop();
    auto& context = KJ_ASSERT_NONNULL(fixture.getActor().getIoContext());
    KJ_EXPECT(KJ_ASSERT_NONNULL(context.getAbortReason()).getDescription().contains(message));
    expectTimeoutCount(0);
  }

  void expectTimeoutCount(size_t expected) {
    auto& context = KJ_ASSERT_NONNULL(fixture.getActor().getIoContext());
    KJ_EXPECT(context.getTimeoutCount() == expected);
  }

 private:
  class Response final: public kj::HttpService::Response {
   public:
    kj::Own<kj::AsyncOutputStream> send(uint statusCode,
        kj::StringPtr statusText,
        const kj::HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize) override {
      this->statusCode = statusCode;
      return kj::heap<kj::NullStream>();
    }

    kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders&) override {
      KJ_UNIMPLEMENTED();
    }

    uint statusCode = 0;
  };

  TestTimerChannel timer;
  kj::PromiseFulfillerPair<void> fetchReady = kj::newPromiseAndFulfiller<void>();
  TestFixture fixture;
  kj::Own<WorkerInterface> entrypoint;
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers;
  kj::NullStream body;
  Response response;
  kj::Promise<void> request;
};

}  // namespace workerd::test
