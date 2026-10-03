// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { ok, strictEqual, throws } from 'node:assert';
import timers from 'node:timers/promises';

// Allow up to 10ms of jitter because the precise_timers compat flag
// can introduce +/-3ms of variance in timer resolution, and coverage
// builds add additional overhead.
const JITTER = 10;

// A setTimeout() with a delay of exactly zero (or omitted, which defaults to zero) called
// outside of a request -- e.g. here, at top-level module scope -- doesn't throw and is
// scheduled as a microtask instead. By the time any request is handled, top-level evaluation
// (and any microtasks it queued) has already run to completion, so `globalScopeTimeoutRan`
// below is observable from a test handler.
// Ref: https://github.com/cloudflare/workerd/issues/389
let globalScopeTimeoutRan = false;
const globalScopeTimeoutId = setTimeout(() => {
  globalScopeTimeoutRan = true;
}, 0);
ok(
  Number.isInteger(globalScopeTimeoutId) && globalScopeTimeoutId > 0,
  'setTimeout() at global scope should return a normal timeout id'
);

// clearTimeout() on such a timeout cancels it before its microtask runs.
let canceledGlobalScopeTimeoutRan = false;
const canceledGlobalScopeTimeoutId = setTimeout(() => {
  canceledGlobalScopeTimeoutRan = true;
}, 0);
clearTimeout(canceledGlobalScopeTimeoutId);

// Any delay other than exactly zero -- positive or negative -- still can't be honored outside
// of a request -- there's no per-request timer queue to schedule it against -- so it continues
// to throw exactly as before.
throws(() => setTimeout(() => {}, 10), /Disallowed operation/);
throws(() => setTimeout(() => {}, -1), /Disallowed operation/);

// setInterval() is intentionally not given the same treatment: a recurring task with no request
// to attach it to could run forever, so it always throws at global scope regardless of delay.
throws(() => setInterval(() => {}, 0), /Disallowed operation/);

export const globalScopeZeroDelayTimeout = {
  async test() {
    ok(
      globalScopeTimeoutRan,
      'zero-delay setTimeout() from global scope should have run by now'
    );
    strictEqual(
      canceledGlobalScopeTimeoutRan,
      false,
      'canceled global-scope timeout should not have run'
    );
  },
};

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
