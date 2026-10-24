// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

async function testKvOperations(state) {
  const storage = state.storage;

  // Test basic put/get operations
  await storage.put('testKey1', 'testValue');
  await storage.put({
    foo1: 'bar1',
    foo2: 'bar2',
    foo3: 'bar3',
    foo4: 'bar4',
  });
  await storage.get('testKey2');
  await storage.get(['testKey1', 'testKey2']);
  await storage.delete('testKey1');
  await storage.list();
  await storage.deleteAll();
  await storage.setAlarm(Date.UTC(2100, 0, 1));
  await storage.getAlarm();
  await storage.deleteAlarm();
  await storage.transaction(() => {
    return 'test';
  });
  await storage.sync();

  // Test failed operations. Depending on the compatibility date, these throw synchronously or
  // return a rejected promise; the async functions handle both.
  await assert.rejects(async () => storage.put('testKey3', undefined), {
    name: 'TypeError',
    message: 'put() called with undefined value.',
  });
  await assert.rejects(
    async () => storage.list({ start: 'a', startAfter: 'b' }),
    {
      name: 'TypeError',
      message: 'list() cannot be called with both start and startAfter values.',
    }
  );
  await assert.rejects(
    async () =>
      storage.transaction(() => {
        throw new Error('Transaction failed');
      }),
    { message: 'Transaction failed' }
  );
  await assert.rejects(async () => storage.snapshot(), {
    message:
      "This Durable Object's storage back-end does not implement snapshots.",
  });
}

export class ActorKvTest extends DurableObject {
  constructor(state, env) {
    super(state, env);
    this.state = state;
  }

  async fetch(req) {
    const url = new URL(req.url);

    if (url.pathname.endsWith('/kv-test')) {
      await testKvOperations(this.state);
      return Response.json({ ok: true, test: 'kv-operations' });
    }

    return Response.json(
      { error: 'Unknown endpoint', path: url.pathname },
      { status: 404 }
    );
  }

  alarm() {}
}

export default {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName('actor-kv-test');
    let obj = env.ns.get(id);

    let doReq = async (path, init = {}) => {
      let resp = await obj.fetch('http://test.example/' + path, init);
      return await resp.json();
    };

    // Test KV operations
    assert.deepEqual(await doReq('kv-test'), {
      ok: true,
      test: 'kv-operations',
    });
  },
};
