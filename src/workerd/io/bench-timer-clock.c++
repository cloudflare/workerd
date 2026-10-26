// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "timer-test-fixture.h"

#include <workerd/tests/bench-tools.h>

namespace workerd {
namespace {

void clockReadsWithBlockedTimers(benchmark::State& state) {
  constexpr uint READS = 10'000;
  auto source = kj::str("const count = ", state.range(0), "; const reads = ", READS, ";\n", R"JS(
    import { DurableObject } from 'cloudflare:workers';
    globalThis.events = '';
    export default class extends DurableObject {
      async fetch() {
        for (let i = 0; i < count; ++i) setTimeout(() => {}, 1);
        await this.ctx.blockConcurrencyWhile(async () => {
          await scheduler.wait(2);
          setTimeout(() => {}, 1000);
          let sum = 0;
          for (let i = 0; i < reads; ++i) sum += performance.now();
          events = String(sum);
          await fetch('https://example.com');
        });
        return new Response('OK');
      }
    }
  )JS");

  for (auto _: state) {
    state.PauseTiming();
    {
      test::TimerTest test(source);
      test.advanceTo(1);
      // Measure clock reads in the child while its own timer remains pending and the root's
      // timers remain blocked. Fixture construction and timer cleanup are outside the sample.
      state.ResumeTiming();
      test.advanceTo(2);
      state.PauseTiming();
      test.expectEvents(kj::str(2 * READS));
    }
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * READS);
}

WD_BENCHMARK(clockReadsWithBlockedTimers)->Arg(0)->Arg(100)->Arg(1000)->Arg(5000)->Iterations(10);

}  // namespace
}  // namespace workerd
