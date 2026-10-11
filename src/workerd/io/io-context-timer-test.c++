// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/io-context.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>
#include <kj/timer.h>

namespace workerd {
namespace {

// Unlike the standalone server's timer, this clock applies the timeout clamp used by embedders
// with Spectre mitigations. Advancing it explicitly also exercises already-overdue deadlines.
class ClampingTimerChannel final: public TimerChannel {
 public:
  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date> nextTimeout) override {
    auto time = kj::UNIX_EPOCH + (timer.now() - kj::origin<kj::TimePoint>());
    return nextTimeout.map([&](kj::Date deadline) {
      return kj::min(time, deadline);
    }).orDefault(time);
  }

  kj::Promise<void> atTime(kj::Date when) override {
    if (when <= now(kj::none)) return kj::READY_NOW;
    return timer.atTime(kj::origin<kj::TimePoint>() + (when - kj::UNIX_EPOCH));
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration) override {
    return kj::NEVER_DONE;
  }

  void advanceTo(uint milliseconds) {
    timer.advanceTo(kj::origin<kj::TimePoint>() + milliseconds * kj::MILLISECONDS);
  }

 private:
  kj::TimerImpl timer{kj::origin<kj::TimePoint>()};
};

// TestFixture uses V8's default Date clock. Scripts read performance.now(), which uses
// IoContext::now() directly, the same clock that the server's V8 platform exposes as Date.now().
class TimerTest {
 public:
  explicit TimerTest(kj::StringPtr source)
      : fixture({.mainModuleSource = source,
          .actorId = Worker::Actor::Id(kj::str("timer-test")),
          .actorClassName = "default"_kj,
          .useRealTimers = false,
          .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
              [this](TimerChannel&) -> kj::Rc<IoChannelFactory> {
                return kj::rc<TestFixture::DummyIoChannelFactory>(timer);
              })}),
        entrypoint(fixture.makeWorkerEntrypoint()),
        headers(headerTable),
        request(
            entrypoint->request(kj::HttpMethod::GET, "https://example.com", headers, body, response)
                .eagerlyEvaluate(nullptr)) {
    fixture.pollEventLoop();
  }

  void advanceTo(uint milliseconds) {
    timer.advanceTo(milliseconds);
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

  ClampingTimerChannel timer;
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

KJ_TEST("a large batch of blocked timers does not prevent a critical section from finishing") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        let completed = 0;
        const outer = Promise.all(Array.from({ length: 2000 }, (_, index) =>
          scheduler.wait(10).then(() => {
            if (completed++ !== index) throw new Error('outside timers ran out of order');
          })));
        await this.ctx.blockConcurrencyWhile(async () => {
          await scheduler.wait(20);
          if (completed !== 0) throw new Error('outside timer entered the critical section');
          events += 'inner;';
        });
        await outer;
        events += `outer:${completed};`;
        return new Response('OK');
      }
    }
  )JS"_kj);

  test.advanceTo(10);
  test.expectEvents("");
  test.advanceTo(20);
  test.expectEvents("inner;outer:2000;");
  test.expectDone();
}

}  // namespace
}  // namespace workerd
