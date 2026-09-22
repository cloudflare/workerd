// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Promise.prototype.then patched by user code: the client, which uses no
// primordials, sends its request and pumps the response through the
// patched then; a transparent patch changes nothing about the data, and a
// patch that throws as the request is sent fails the request.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { request, collect, once, record } from 'harness';

async function withPatchedThen(patch, fn) {
  const original = Promise.prototype.then;
  Promise.prototype.then = function (onFulfilled, onRejected) {
    return patch.call(this, original, onFulfilled, onRejected);
  };
  try {
    await fn();
  } finally {
    Promise.prototype.then = original;
  }
}

// A transparent then sees the exchange's hops (the count is not pinned);
// the request body and the response body are intact.
export const patchedThenPassthroughKeepsData = {
  async test(ctrl, env) {
    let calls = 0;
    let echoed;
    await withPatchedThen(
      function (original, onFulfilled, onRejected) {
        calls++;
        return original.call(this, onFulfilled, onRejected);
      },
      async () => {
        const req = request(env, '/echo', { method: 'POST' });
        req.end('ping');
        const res = await once(req, 'response');
        echoed = (await collect(res)).toString();
      }
    );
    strictEqual(echoed, 'ping');
    strictEqual(calls > 0, true);
  },
};

// A then that throws once, as the request is being sent (armed from a
// 'finish' listener that runs before the client's own): the request fails
// with that error — 'error', 'close', no 'response' — rather than the
// throw escaping the 'finish' emission and the request never being sent.
export const hostileThenDuringSendFailsRequest = {
  async test(ctrl, env) {
    const log = [];
    const boom = new Error('hostile then');
    let armed = false;
    await withPatchedThen(
      function (original, onFulfilled, onRejected) {
        if (armed) {
          armed = false;
          throw boom;
        }
        return original.call(this, onFulfilled, onRejected);
      },
      async () => {
        const req = request(env, '/asd');
        record(log, 'req', req, ['response', 'error', 'close']);
        req.prependListener('finish', () => {
          armed = true;
        });
        req.end();
        await once(req, 'close');
        strictEqual(armed, false);
      }
    );
    deepStrictEqual(log, ['req:error(Error/-/hostile then)', 'req:close']);
  },
};
