// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Timer ids, clearTimeout() of ids that name no active timer, and the active timeout quota
// (IoContext::TimeoutManagerImpl). The globals here are workerd's own, not node:timers'.
import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';

const QUOTA = 10_000;

function quotaMessage(active, finished) {
  return (
    'You have exceeded the number of active timeouts you may set. ' +
    `max active timeouts: ${QUOTA}, current active timeouts: ${active}, ` +
    `finished timeouts: ${finished}`
  );
}

// Each test event runs in a new IoContext on the same global scope, which owns the id generator.
let idFromEarlierRequest;

export const idsArePositiveIntegersThatIncrease = {
  test() {
    const ids = [
      setTimeout(() => {}, 1000),
      setInterval(() => {}, 1000),
      setTimeout(() => {}, 0),
    ];
    for (const id of ids) {
      ok(Number.isSafeInteger(id) && id > 0, `${id}`);
      clearTimeout(id);
    }
    deepStrictEqual(ids, [ids[0], ids[0] + 1, ids[0] + 2]);
    idFromEarlierRequest = ids[2];
  },
};

export const idsKeepIncreasingAcrossRequests = {
  test() {
    const id = setTimeout(() => {});
    ok(id > idFromEarlierRequest, `${id} <= ${idFromEarlierRequest}`);
    clearTimeout(id);
  },
};

export const clearingIdsThatNameNoActiveTimerIsANoOp = {
  async test() {
    let fired = false;
    const id = setTimeout(() => (fired = true), 0);
    for (const unknown of [
      undefined,
      null,
      0,
      1.5,
      NaN,
      Infinity,
      2 ** 53,
      2 ** 60,
      idFromEarlierRequest,
      id + 1,
    ]) {
      clearTimeout(unknown);
      clearInterval(unknown);
    }
    await scheduler.wait(10);
    ok(fired);
    // An id that has already fired.
    clearTimeout(id);
  },
};

export const aCallbackCanClearATimerDueAtTheSameTime = {
  async test() {
    const events = [];
    let second;
    setTimeout(() => {
      events.push('first');
      clearTimeout(second);
    }, 5);
    second = setTimeout(() => events.push('second'), 5);
    await scheduler.wait(20);
    deepStrictEqual(events, ['first']);
  },
};

export const clearedAndDisposedImmediatesFreeTheirQuotaSlots = {
  test() {
    const pending = [];
    for (let i = 0; i < QUOTA - 1; i++)
      pending.push(setTimeout(() => {}, 1000));
    clearImmediate(setImmediate(() => {}));
    clearImmediate(setImmediate(() => {}));
    setImmediate(() => {})[Symbol.dispose]();
    clearTimeout(setTimeout(() => {}, 1000));
    pending.push(setTimeout(() => {}, 1000));
    throws(() => setImmediate(() => {}), { message: quotaMessage(QUOTA, 4) });
    throws(() => setTimeout(() => {}), { message: quotaMessage(QUOTA, 4) });
    for (const id of pending) clearTimeout(id);
    strictEqual(typeof setTimeout(() => {}), 'number');
  },
};
