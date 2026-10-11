// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>
#include <kj/timer.h>

namespace workerd {
namespace {

enum class TimerClock {
  // Applies the timeout clamp used by embedders with Spectre mitigations. Advancing the clock
  // explicitly also exercises already-overdue deadlines.
  CLAMPED,
  // Like the standalone server's timer, now() ignores the timeout clamp and reads a wall clock,
  // which this test never advances, while deadlines wait on the advancing timer.
  UNCLAMPED,
};

class TestTimerChannel final: public TimerChannel {
 public:
  explicit TestTimerChannel(TimerClock clock): clock(clock) {}

  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date> nextTimeout) override {
    if (clock == TimerClock::UNCLAMPED) return kj::UNIX_EPOCH;
    auto time = timerTime();
    return nextTimeout.map([&](kj::Date deadline) {
      return kj::min(time, deadline);
    }).orDefault(time);
  }

  kj::Promise<void> atTime(kj::Date when) override {
    // Like the standalone server, measure the delay from now() and wait for it on the timer.
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

 private:
  TimerClock clock;
  kj::TimerImpl timer{kj::origin<kj::TimePoint>()};

  kj::Date timerTime() {
    return kj::UNIX_EPOCH + (timer.now() - kj::origin<kj::TimePoint>());
  }
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

KJ_TEST("critical section timers advance a clamped clock past a blocked outside timer") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const outer = scheduler.wait(10).then(() => events += `outer:${performance.now()};`);
        await this.ctx.blockConcurrencyWhile(async () => {
          await scheduler.wait(200);
          events += `first:${performance.now()};`;
          await scheduler.wait(100);
          events += `second:${performance.now()};`;
        });
        await outer;
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(10);
  test.expectEvents("");
  test.advanceTo(200);
  test.expectEvents("first:200;");
  test.advanceTo(299);
  test.expectEvents("first:200;");
  test.advanceTo(300);
  test.expectEvents("first:200;second:300;outer:300;");
  test.expectDone();
}

KJ_TEST("independent I/O advances the clock after an overtaking timer completes") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const outer = scheduler.wait(10).then(() => events += `outer:${performance.now()};`);
        await this.ctx.blockConcurrencyWhile(async () => {
          await scheduler.wait(200);
          events += `timer:${performance.now()};`;
          await fetch('https://example.com');
          events += `io:${performance.now()};`;
          await scheduler.wait(100);
          events += `after:${performance.now()};`;
        });
        await outer;
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(10);
  test.expectEvents("");
  test.advanceTo(200);
  test.expectEvents("timer:200;");
  test.advanceTo(300);
  test.expectEvents("timer:200;");
  test.completeFetch();
  test.expectEvents("timer:200;io:300;");
  test.advanceTo(399);
  test.expectEvents("timer:200;io:300;");
  test.advanceTo(400);
  test.expectEvents("timer:200;io:300;after:400;outer:400;");
  test.expectDone();
}

KJ_TEST("equal-deadline timers on different gates clamp their own callbacks") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const outer = scheduler.wait(20).then(() => events += `outer:${performance.now()};`);
        await this.ctx.blockConcurrencyWhile(async () => {
          await scheduler.wait(20);
          events += `timer:${performance.now()};`;
          await fetch('https://example.com');
          events += `io:${performance.now()};`;
          await scheduler.wait(100);
          events += `after:${performance.now()};`;
        });
        await outer;
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(19);
  test.expectEvents("");
  test.advanceTo(20);
  test.expectEvents("timer:20;");
  test.advanceTo(100);
  test.expectEvents("timer:20;");
  test.completeFetch();
  test.expectEvents("timer:20;io:100;");
  test.advanceTo(199);
  test.expectEvents("timer:20;io:100;");
  test.advanceTo(200);
  test.expectEvents("timer:20;io:100;after:200;outer:200;");
  test.expectDone();
}

KJ_TEST("critical section timers overtake outside timers while independent I/O is pending") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const outer = scheduler.wait(10).then(() => events += `outer:${performance.now()};`);
        let inner;
        await this.ctx.blockConcurrencyWhile(async () => {
          inner = scheduler.wait(20).then(() => events += `inner:${performance.now()};`);
          await fetch('https://example.com');
          events += `section:${performance.now()};`;
        });
        await Promise.all([outer, inner]);
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(10);
  test.expectEvents("");
  test.advanceTo(20);
  test.expectEvents("inner:20;");
  test.advanceTo(100);
  test.expectEvents("inner:20;");
  test.completeFetch();
  test.expectEvents("inner:20;section:100;outer:100;");
  test.expectDone();
}

KJ_TEST("overdue timers on the same gate retain their order and clamped callback times") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        await Promise.all([10, 20, 20, 30].map((delay, index) => new Promise(resolve => {
          setTimeout(() => {
            events += `${index}:${performance.now()};`;
            resolve();
          }, delay);
        })));
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(100);
  test.expectEvents("0:10;1:20;2:20;3:30;");
  test.expectDone();
}

KJ_TEST("canceling a fired blocked timer preserves the next critical section timer") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const outer = setTimeout(() => events += 'canceled;', 10);
        await this.ctx.blockConcurrencyWhile(async () => {
          await scheduler.wait(20);
          clearTimeout(outer);
          await scheduler.wait(10);
          events += `inner:${performance.now()};`;
        });
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(10);
  test.expectEvents("");
  test.advanceTo(20);
  test.expectEvents("");
  test.advanceTo(30);
  test.expectEvents("inner:30;");
  test.expectDone();
}

KJ_TEST("canceling an armed timer and inserting an earlier timer rearms the queue") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const canceled = setTimeout(() => events += 'canceled;', 10);
        const last = scheduler.wait(30).then(() => events += `last:${performance.now()};`);
        clearTimeout(canceled);
        await scheduler.wait(20);
        events += `first:${performance.now()};`;
        await last;
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(10);
  test.expectEvents("");
  test.advanceTo(20);
  test.expectEvents("first:20;");
  test.advanceTo(30);
  test.expectEvents("first:20;last:30;");
  test.expectDone();
}

KJ_TEST("canceling a blocked timer after independent I/O preserves the next wait") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const outer = setTimeout(() => events += 'canceled;', 10);
        await this.ctx.blockConcurrencyWhile(async () => {
          await fetch('https://example.com');
          clearTimeout(outer);
          events += `io:${performance.now()};`;
          await scheduler.wait(100);
          events += `after:${performance.now()};`;
        });
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(10);
  test.expectEvents("");
  test.advanceTo(300);
  test.completeFetch();
  test.expectEvents("io:300;");
  test.advanceTo(399);
  test.expectEvents("io:300;");
  test.advanceTo(400);
  test.expectEvents("io:300;after:400;");
  test.expectDone();
}

KJ_TEST("nested critical sections advance past their parents' blocked timers") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const outer = scheduler.wait(10).then(() => events += `outer:${performance.now()};`);
        await this.ctx.blockConcurrencyWhile(async () => {
          const middle = scheduler.wait(20).then(() => events += `middle:${performance.now()};`);
          await this.ctx.blockConcurrencyWhile(async () => {
            await scheduler.wait(30);
            events += `inner:${performance.now()};`;
          });
          await middle;
        });
        await outer;
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(10);
  test.expectEvents("");
  test.advanceTo(20);
  test.expectEvents("");
  test.advanceTo(30);
  test.expectEvents("inner:30;middle:30;outer:30;");
  test.expectDone();
}

KJ_TEST("intervals inside a critical section keep advancing past an outside timer") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const outer = scheduler.wait(5);
        await this.ctx.blockConcurrencyWhile(() => new Promise(resolve => {
          let count = 0;
          const interval = setInterval(() => {
            events += `${performance.now()};`;
            if (++count === 3) {
              clearInterval(interval);
              resolve();
            }
          }, 10);
        }));
        await outer;
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(5);
  test.expectEvents("");
  test.advanceTo(10);
  test.expectEvents("10;");
  test.advanceTo(20);
  test.expectEvents("10;20;");
  test.advanceTo(30);
  test.expectEvents("10;20;30;");
  test.expectDone();
}

KJ_TEST("intervals keep their cadence when the timer channel ignores the clock clamp") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        await new Promise(resolve => {
          let count = 0;
          const interval = setInterval(() => {
            events += 'tick;';
            if (++count === 3) {
              clearInterval(interval);
              resolve();
            }
          }, 10);
        });
        return new Response('OK');
      }
    }
  )JS"_kj,
      TimerClock::UNCLAMPED);

  test.advanceTo(10);
  test.expectEvents("tick;");
  test.advanceTo(20);
  test.expectEvents("tick;tick;");
  test.advanceTo(30);
  test.expectEvents("tick;tick;tick;");
  test.expectDone();
}

KJ_TEST("timers from a completed critical section retain their order on the parent gate") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        let adopted;
        await this.ctx.blockConcurrencyWhile(async () => {
          adopted = scheduler.wait(10).then(() => events += `adopted:${performance.now()};`);
        });
        const outer = scheduler.wait(20).then(() => events += `outer:${performance.now()};`);
        await this.ctx.blockConcurrencyWhile(async () => {
          await scheduler.wait(30);
          events += `inner:${performance.now()};`;
        });
        await Promise.all([adopted, outer]);
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(10);
  test.expectEvents("");
  test.advanceTo(20);
  test.expectEvents("");
  test.advanceTo(30);
  test.expectEvents("inner:30;adopted:30;outer:30;");
  test.expectDone();
}

KJ_TEST("independent I/O before the first inside timer advances time") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const outer = scheduler.wait(10);
        await this.ctx.blockConcurrencyWhile(async () => {
          await fetch('https://example.com');
          events += `io:${performance.now()};`;
          await scheduler.wait(100);
          events += `after:${performance.now()};`;
        });
        await outer;
        return new Response('OK');
      }
    }
  )JS"_kj);
  test.advanceTo(300);
  test.completeFetch();
  test.expectEvents("io:300;");
  test.advanceTo(399);
  test.expectEvents("io:300;");
  test.advanceTo(400);
  test.expectEvents("io:300;after:400;");
  test.expectDone();
}

KJ_TEST("another outside deadline does not reclamp I/O after an inside timer") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const first = scheduler.wait(10);
        const second = scheduler.wait(250);
        await this.ctx.blockConcurrencyWhile(async () => {
          await scheduler.wait(200);
          events += `timer:${performance.now()};`;
          await fetch('https://example.com');
          events += `io:${performance.now()};`;
          await scheduler.wait(100);
          events += `after:${performance.now()};`;
        });
        await Promise.all([first, second]);
        return new Response('OK');
      }
    }
  )JS"_kj);
  test.advanceTo(10);
  test.advanceTo(200);
  test.expectEvents("timer:200;");
  test.advanceTo(300);
  test.completeFetch();
  test.expectEvents("timer:200;io:300;");
  test.advanceTo(350);
  test.expectEvents("timer:200;io:300;");
  test.advanceTo(400);
  test.expectEvents("timer:200;io:300;after:400;");
  test.expectDone();
}

KJ_TEST("setTimeout inserted by an overdue callback retains deadline order") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const first = new Promise(resolve => {
          setTimeout(() => {
            events += `first:${performance.now()};`;
            setTimeout(() => {
              events += `inserted:${performance.now()};`;
              resolve();
            }, 10);
          }, 10);
        });
        const last = new Promise(resolve => {
          setTimeout(() => {
            events += `last:${performance.now()};`;
            resolve();
          }, 30);
        });
        await Promise.all([first, last]);
        return new Response('OK');
      }
    }
  )JS"_kj);
  test.advanceTo(100);
  test.expectEvents("first:10;inserted:20;last:30;");
  test.expectDone();
}

KJ_TEST("nested timers progress after their parent critical section completes") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const outside = scheduler.wait(10).then(() => events += `outside:${performance.now()};`);
        let nested;
        await this.ctx.blockConcurrencyWhile(() => new Promise(resolve => {
          nested = this.ctx.blockConcurrencyWhile(async () => {
            resolve();
            await scheduler.wait(30);
            events += `nested:${performance.now()};`;
          });
        }));
        events += `parent:${performance.now()};`;
        await nested;
        await outside;
        return new Response('OK');
      }
    }
  )JS"_kj);
  test.expectEvents("parent:0;");
  test.advanceTo(10);
  test.expectEvents("parent:0;");
  test.advanceTo(30);
  test.expectEvents("parent:0;nested:30;outside:30;");
  test.expectDone();
}

KJ_TEST("completed critical section intervals merge and reschedule on the parent gate") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        let intervalDone, canceled;
        await this.ctx.blockConcurrencyWhile(() => {
          canceled = setTimeout(() => events += 'canceled;', 5);
          intervalDone = new Promise(resolve => {
            let count = 0;
            const id = setInterval(() => {
              events += `interval:${performance.now()};`;
              if (++count === 3) {
                clearInterval(id);
                resolve();
              }
            }, 10);
          });
        });
        clearTimeout(canceled);
        const parent = new Promise(resolve => setTimeout(() => {
          events += `parent:${performance.now()};`;
          resolve();
        }, 15));
        await Promise.all([intervalDone, parent]);
        return new Response('OK');
      }
    }
  )JS"_kj);
  test.advanceTo(100);
  test.expectEvents("interval:10;parent:15;interval:20;interval:30;");
  test.expectDone();
}

KJ_TEST("merging fired timers preserves callbacks and supports cancellation after handoff") {
  struct {
    kj::StringPtr canceled;
    kj::StringPtr expected;
  } cases[] = {
    {"none", "parent:40;nested:60;outside:60;adopted:60;parent-next:60;child-next:60;"},
    {"outside", "parent:40;nested:60;adopted:60;parent-next:60;child-next:60;"},
    {"adopted", "parent:40;nested:60;outside:60;parent-next:60;child-next:60;"},
  };
  for (auto& testCase: cases) {
    KJ_CONTEXT(testCase.canceled);
    auto source = kj::str("const canceled = '", testCase.canceled, "';\n", R"JS(
      import { DurableObject } from 'cloudflare:workers';
      globalThis.events = '';
      export default class extends DurableObject {
        async fetch() {
          const timers = {};
          timers.outside = setTimeout(() => events += `outside:${performance.now()};`, 10);
          setTimeout(() => events += `parent-next:${performance.now()};`, 30);
          let nested;
          await this.ctx.blockConcurrencyWhile(() => new Promise(resolve => {
            timers.adopted = setTimeout(() => events += `adopted:${performance.now()};`, 20);
            setTimeout(() => events += `child-next:${performance.now()};`, 40);
            nested = this.ctx.blockConcurrencyWhile(async () => {
              await fetch('https://example.com');
              resolve();
              await scheduler.wait(20);
              events += `nested:${performance.now()};`;
            });
          }));
          events += `parent:${performance.now()};`;
          if (canceled !== 'none') clearTimeout(timers[canceled]);
          await nested;
          await scheduler.wait(0);
          return new Response('OK');
        }
      }
    )JS");
    TimerTest test(source);

    // Both scopes have fired their first timer, but the nested section keeps their callbacks
    // blocked. Completing fetch succeeds the enclosing section and merges its queue into root.
    test.advanceTo(10);
    test.expectEvents("");
    test.advanceTo(20);
    test.expectEvents("");
    test.advanceTo(40);
    test.completeFetch();
    test.expectEvents("parent:40;");
    test.advanceTo(59);
    test.expectEvents("parent:40;");
    test.advanceTo(60);
    test.expectEvents(testCase.expected);
    test.expectDone();
  }
}

KJ_TEST("adopted fired callbacks retain gate order ahead of earlier unarmed parent timers") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const first = new Promise(resolve => setTimeout(() => {
          events += `root-100:${performance.now()};`;
          resolve();
        }, 100));
        const second = new Promise(resolve => setTimeout(() => {
          events += `root-150:${performance.now()};`;
          resolve();
        }, 150));
        let nested, adopted, releaseNested;
        const holdNested = new Promise(resolve => { releaseNested = resolve; });
        await this.ctx.blockConcurrencyWhile(() => new Promise(resolve => {
          adopted = new Promise(done => setTimeout(() => {
            events += `child-200:${performance.now()};`;
            done();
          }, 200));
          nested = this.ctx.blockConcurrencyWhile(async () => {
            await fetch('https://example.com');
            events += `io:${performance.now()};`;
            resolve();
            await holdNested;
          });
        }));
        events += `parent:${performance.now()};`;
        releaseNested();
        await Promise.all([nested, first, second, adopted]);
        await scheduler.wait(50);
        events += `after:${performance.now()};`;
        return new Response('OK');
      }
    }
  )JS"_kj);

  // Root's first timer and the child's timer reach their gates; root's second timer is unarmed.
  test.advanceTo(100);
  test.advanceTo(150);
  test.advanceTo(200);
  test.expectEvents("");
  test.advanceTo(400);
  // Independent I/O lets the child complete while the nested section still holds its gate.
  // Adoption preserves the fired child's waiter ahead of root's earlier unarmed deadline.
  test.completeFetch();
  auto expected = "io:400;parent:400;root-100:400;child-200:400;root-150:400;"_kj;
  test.expectEvents(expected);
  test.advanceTo(449);
  test.expectEvents(expected);
  test.advanceTo(450);
  test.expectEvents(kj::str(expected, "after:450;"));
  test.expectDone();
}

KJ_TEST("adopted fired callbacks never observe time before their own deadline") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const first = new Promise(resolve => setTimeout(() => {
          events += `root-100:${performance.now()};`;
          resolve();
        }, 100));
        const second = new Promise(resolve => setTimeout(() => {
          events += `root-150:${performance.now()};`;
          resolve();
        }, 150));
        let nested, adopted, releaseNested;
        const holdNested = new Promise(resolve => { releaseNested = resolve; });
        await this.ctx.blockConcurrencyWhile(() => new Promise(resolve => {
          adopted = new Promise(done => setTimeout(() => {
            events += `child-200:${performance.now()};`;
            setTimeout(() => {
              events += `after:${performance.now()};`;
              done();
            }, 300);
          }, 200));
          nested = this.ctx.blockConcurrencyWhile(async () => {
            await fetch('https://example.com');
            events += 'io;';
            resolve();
            await holdNested;
          });
        }));
        events += 'parent;';
        releaseNested();
        await Promise.all([nested, first, second, adopted]);
        return new Response('OK');
      }
    }
  )JS"_kj);

  // Root's first timer and the child's timer reach their gates; root's second timer is unarmed.
  test.advanceTo(100);
  test.advanceTo(150);
  test.advanceTo(200);
  test.expectEvents("");
  test.advanceTo(400);
  // No clock is read during I/O completion: each delivered timer must establish its own deadline
  // as a floor, including the adopted callback that precedes root's earlier unarmed deadline.
  test.completeFetch();
  auto expected = "io;parent;root-100:100;child-200:200;root-150:200;"_kj;
  test.expectEvents(expected);
  // The adopted callback's follow-up timer is relative to its own deadline, not root's 150ms.
  test.advanceTo(499);
  test.expectEvents(expected);
  test.advanceTo(500);
  test.expectEvents(kj::str(expected, "after:500;"));
  test.expectDone();
}

KJ_TEST("parent callbacks cannot rewind time after independent critical section I/O") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        const outside = new Promise(resolve => setTimeout(() => {
          events += `outside:${performance.now()};`;
          setTimeout(() => {
            events += `after:${performance.now()};`;
            resolve();
          }, 100);
        }, 10));
        await this.ctx.blockConcurrencyWhile(async () => {
          await fetch('https://example.com');
          events += `io:${performance.now()};`;
        });
        await outside;
        return new Response('OK');
      }
    }
  )JS"_kj);
  test.advanceTo(10);
  test.advanceTo(300);
  test.completeFetch();
  test.expectEvents("io:300;outside:300;");
  test.advanceTo(399);
  test.expectEvents("io:300;outside:300;");
  test.advanceTo(400);
  test.expectEvents("io:300;outside:300;after:400;");
  test.expectDone();
}

KJ_TEST("overdue timers in a single critical section retain their clock clamp") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        await this.ctx.blockConcurrencyWhile(() => new Promise(done => {
          setTimeout(() => {
            events += `first:${performance.now()};`;
            setTimeout(() => {
              events += `second:${performance.now()};`;
              done();
            }, 10);
          }, 30);
        }));
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(100);
  test.expectEvents("first:30;second:40;");
  test.expectDone();
}

KJ_TEST("clock reads outside the current context do not advance its observable clock floor") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        await this.ctx.blockConcurrencyWhile(() => new Promise(done => {
          setTimeout(() => {
            events += `first:${performance.now()};`;
            setTimeout(() => {
              events += `second:${performance.now()};`;
              done();
            }, 10);
          }, 30);
        }));
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceWithoutPolling(100);
  test.expectTimeOutsideContext(100);
  test.advanceTo(100);
  test.expectEvents("first:30;second:40;");
  test.expectDone();
}

KJ_TEST("a failed critical section cancels fired and armed timers across input-gate scopes") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        setTimeout(() => events += 'root-fired;', 10);
        setTimeout(() => events += 'root-pending;', 100);
        await this.ctx.blockConcurrencyWhile(async () => {
          setTimeout(() => events += 'child-fired;', 20);
          setTimeout(() => events += 'child-pending;', 200);
          await this.ctx.blockConcurrencyWhile(async () => {
            setTimeout(() => events += 'nested-armed;', 300);
            await fetch('https://example.com');
            events += 'failure;';
            throw new Error('timer test failure');
          });
        });
        return new Response('unexpected success');
      }
    }
  )JS"_kj);

  // Root and child callbacks are waiting on their gates; their next deadlines are unarmed.
  // The nested section still has an armed timer when its independent I/O completes and it fails.
  test.advanceTo(20);
  test.expectEvents("");
  test.expectTimeoutCount(5);
  test.completeFetch();
  test.expectFailed("timer test failure");
  test.expectEvents("failure;");
  test.advanceTo(500);
  test.expectTimeoutCount(0);
  test.expectEvents("failure;");
}

KJ_TEST("overdue nested timers retain their clock clamp after parent completion") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        let nested;
        await this.ctx.blockConcurrencyWhile(() => new Promise(resolve => {
          nested = this.ctx.blockConcurrencyWhile(async () => {
            resolve();
            await new Promise(done => setTimeout(() => {
              events += `first:${performance.now()};`;
              setTimeout(() => {
                events += `second:${performance.now()};`;
                done();
              }, 10);
            }, 30));
          });
        }));
        events += `parent:${performance.now()};`;
        await nested;
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.expectEvents("parent:0;");
  test.advanceTo(100);
  test.expectEvents("parent:0;first:30;second:40;");
  test.expectDone();
}

}  // namespace
}  // namespace workerd
