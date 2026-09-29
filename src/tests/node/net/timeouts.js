// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// setTimeout(): the idle timer fires 'timeout' (without closing the socket)
// when neither the read loop delivers nor the writer flushes for the
// duration; delivered data resets it; setTimeout(0) clears it.

import { ok, strictEqual } from 'node:assert';
import { echo, ticker, once, readAtLeast } from 'servers';

// An idle socket times out; the socket stays open and usable.
export const idleSocketTimesOut = {
  async test(ctrl, env) {
    const socket = echo(env);
    await once(socket, 'connect');
    let timeouts = 0;
    socket.setTimeout(30, () => timeouts++);
    await once(socket, 'timeout');
    strictEqual(timeouts, 1);
    strictEqual(socket.destroyed, false);
    const echoed = readAtLeast(socket, 'still here'.length);
    socket.write('still here');
    strictEqual((await echoed).toString(), 'still here');
    socket.end();
    await once(socket, 'close');
  },
};

// setTimeout(0) cancels a pending timer, and the socket stays without one:
// activity afterwards (a write, data arriving) restarts nothing.
export const zeroClearsTimeout = {
  async test(ctrl, env) {
    const socket = echo(env);
    await once(socket, 'connect');
    let timeouts = 0;
    socket.on('timeout', () => timeouts++);
    socket.setTimeout(30);
    socket.setTimeout(0);
    await scheduler.wait(80);
    strictEqual(timeouts, 0);
    const echoed = readAtLeast(socket, 'still untimed'.length);
    socket.write('still untimed');
    strictEqual((await echoed).toString(), 'still untimed');
    await scheduler.wait(80);
    strictEqual(timeouts, 0);
    socket.end();
    await once(socket, 'close');
  },
};

// Data arriving from the peer keeps re-arming the timer: a 'timeout' can
// only follow a stretch of at least the timeout without a delivery. The
// peer ticks every 20 ms, so normally none fires within the 250 ms; a
// machine stalled for 80 ms mid-test does make one fire (the expired timer
// runs before the reads it delayed), and that one is correct — the test
// asserts the gap rather than failing on the stall.
export const incomingDataResetsTimeout = {
  async test(ctrl, env) {
    const TIMEOUT = 80;
    const socket = ticker(env);
    await once(socket, 'connect');
    let ticks = 0;
    let lastDelivery = Date.now();
    socket.on('data', () => {
      ticks++;
      lastDelivery = Date.now();
    });
    const gaps = [];
    socket.on('timeout', () => gaps.push(Date.now() - lastDelivery));
    socket.setTimeout(TIMEOUT);
    socket.resume();
    await scheduler.wait(250);
    ok(ticks >= 2, `only ${ticks} tick(s) arrived`);
    for (const gap of gaps) {
      // Integer-millisecond clock reads can shave 1 ms off the gap.
      ok(gap >= TIMEOUT - 1, `'timeout' fired ${gap} ms after a delivery`);
    }
    socket.end();
    await once(socket, 'close');
  },
};
