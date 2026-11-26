// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Sends scheduled events in the cases that http-test does not cover: a handler that rejects or
// uses ctx.waitUntil(), a Worker without scheduled(), and Service Workers syntax, with and without
// a listener.

import assert from 'node:assert';

async function check(service, cron, expected) {
  const result = await service.scheduled({ scheduledTime: 1000, cron });
  assert.deepStrictEqual(
    { outcome: result.outcome, noRetry: result.noRetry },
    expected,
    `cron ${cron}`
  );
}

const OK = { outcome: 'ok', noRetry: false };
const EXCEPTION = { outcome: 'exception', noRetry: false };
// A missing handler fails the event, which is then not retried.
const NO_HANDLER = { outcome: 'exception', noRetry: true };

export default {
  async test(ctrl, env) {
    await check(env.HANDLER, 'reject', EXCEPTION);
    await check(env.HANDLER, 'wait-until', OK);

    await check(env.NO_HANDLER, 'ok', NO_HANDLER);

    await check(env.LISTENER, 'ok', OK);
    await check(env.LISTENER, 'no-retry', { outcome: 'ok', noRetry: true });
    await check(env.LISTENER, 'throw', EXCEPTION);
    await check(env.LISTENER, 'wait-until', OK);

    await check(env.NO_LISTENER, 'ok', NO_HANDLER);
  },
};
