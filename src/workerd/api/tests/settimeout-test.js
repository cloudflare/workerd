import { ok } from 'node:assert';
import timers from 'node:timers/promises';

// Allow up to 10ms of jitter because the precise_timers compat flag
// can introduce +/-3ms of variance in timer resolution, and coverage
// builds add additional overhead.
const JITTER = 10;

// The first setTimeout was firing too early because
// kj::Timer::now() was stale after script compilation/startup.
// Ref: https://github.com/cloudflare/workerd/issues/6019
export const basicAccuracy = {
  async test() {
    const t0 = Date.now();
    await timers.setTimeout(100);
    const t1 = Date.now();
    ok(t1 - t0 >= 100 - JITTER, `Received a difference of ${t1 - t0}`);
    await timers.setTimeout(100);
    const t2 = Date.now();
    ok(t2 - t1 >= 100 - JITTER, `Received a difference of ${t2 - t1}`);
  },
};

// After CPU-heavy work between setTimeout calls,
// subsequent consecutive setTimeouts must still each wait their full delay
// rather than all firing at the same instant.
// Ref: https://github.com/cloudflare/workerd/issues/6037
export const accuracyAfterCpuWork = {
  async test() {
    await timers.setTimeout(50);

    // Let's burn some CPU
    for (let j = 0; j < 1e9; j++);

    const a = Date.now();
    await timers.setTimeout(50);
    const b = Date.now();
    ok(
      b - a >= 50 - JITTER,
      `After CPU work, first sleep: expected ~50ms, got ${b - a}ms`
    );

    await timers.setTimeout(50);
    const c = Date.now();
    ok(
      c - b >= 50 - JITTER,
      `After CPU work, second sleep: expected ~50ms, got ${c - b}ms`
    );

    await timers.setTimeout(50);
    const d = Date.now();
    ok(
      d - c >= 50 - JITTER,
      `After CPU work, third sleep: expected ~50ms, got ${d - c}ms`
    );
  },
};
