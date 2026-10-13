// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "timer-test-fixture.h"

namespace workerd {
namespace {

using test::TimerTest;

KJ_TEST("a queue change retries a deadline whose timer wait failed to arm") {
  TimerTest test(R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        setTimeout(() => events += 'first;', 10);
        const second = scheduler.wait(20).then(() => events += 'second;');
        await fetch('https://example.com');
        const last = scheduler.wait(30).then(() => events += 'last;');
        await Promise.all([second, last]);
        return new Response('OK');
      }
    }
  )JS"_kj);

  // Completing the first callback removes its deadline and attempts to arm the second.
  test.failNextWait();
  {
    KJ_EXPECT_LOG(ERROR, "injected timer failure");
    test.advanceTo(10);
  }
  test.expectEvents("first;");
  // Independent I/O inserts a later deadline. The earlier failed wait must be retried.
  test.completeFetch();
  test.advanceTo(20);
  test.expectEvents("first;second;");
  test.advanceTo(40);
  test.expectEvents("first;second;last;");
  test.expectDone();
}

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
            events += `tick:${performance.now()};`;
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
      test::TimerClock::UNCLAMPED);

  test.advanceTo(10);
  test.expectEvents("tick:0;");
  test.advanceTo(20);
  test.expectEvents("tick:0;tick:0;");
  test.advanceTo(30);
  test.expectEvents("tick:0;tick:0;tick:0;");
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

KJ_TEST("adopted callbacks and their microtasks retain the delivered clock") {
  for (auto dispatch: {"(callback) => callback()"_kj, "queueMicrotask"_kj,
         "async (callback) => { await Promise.resolve(); callback(); }"_kj}) {
    TimerTest test(kj::str("const dispatch = ", dispatch, ";\n", R"JS(
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
          adopted = new Promise(done => setTimeout(() => dispatch(() => {
            events += `child-200:${performance.now()};`;
            setTimeout(() => {
              events += `after:${performance.now()};`;
              done();
            }, 300);
          }), 200));
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
  )JS"_kj));

    // Root's first timer and the child's timer reach their gates; root's second timer is unarmed.
    test.advanceTo(100);
    test.advanceTo(150);
    test.advanceTo(200);
    test.expectEvents("");
    test.advanceTo(400);
    // No clock is read during I/O completion: each delivered timer must establish its own deadline
    // as a floor through its microtasks, even before root's earlier unarmed deadline.
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
