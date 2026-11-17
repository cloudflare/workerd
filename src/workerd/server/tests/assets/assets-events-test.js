// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export const rpc = {
  async test(ctrl, env) {
    assert.strictEqual(await env.ROUTER.hello('router'), 'hello, router');
  },
};

export const scheduled = {
  async test(ctrl, env) {
    const result = await env.ROUTER.scheduled({ cron: '* * * * 1' });
    assert.strictEqual(result.outcome, 'ok');
    assert.strictEqual(await env.ROUTER.scheduledCron(), '* * * * 1');
  },
};

export const tail = {
  async test(ctrl, env) {
    const response = await env.PRODUCER.fetch('http://producer/');
    assert.strictEqual(await response.text(), 'ok');
    assert.deepStrictEqual(await env.ROUTER.tailedLogs(), ['produced']);
  },
};

export const fetchRoutes = {
  async test(ctrl, env) {
    const missing = await env.ROUTER.fetch('http://app/missing');
    assert.strictEqual(await missing.text(), 'from the user Worker');
  },
};

export const serveOnlyDoesNotForward = {
  async test(ctrl, env) {
    const missing = await env.ASSETS.fetch('http://app/missing');
    assert.strictEqual(missing.status, 404);
    await assert.rejects(env.ASSETS.hello('assets'));
    await assert.rejects(env.ASSETS.scheduled({ cron: '* * * * 1' }));
  },
};
