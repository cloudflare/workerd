// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

async function getLastRestartBody(env, id) {
  const res = await env.mock.fetch('http://placeholder/last-restart', {
    method: 'POST',
    body: JSON.stringify({ id }),
  });
  return (await res.json()).result;
}

export const tests = {
  async test(_, env) {
    {
      // Test create instance
      const instance = await env.workflow.create({
        id: 'foo',
        payload: { bar: 'baz' },
      });
      assert.deepStrictEqual(instance.id, 'foo');
    }

    {
      // Test get instance
      const instance = await env.workflow.get('bar');
      assert.deepStrictEqual(instance.id, 'bar');
    }

    {
      // Test createBatch
      const instances = await env.workflow.createBatch([
        {
          id: 'foo',
          payload: { bar: 'baz' },
        },
        {
          id: 'bar',
          payload: { bar: 'baz' },
        },
      ]);
      assert.deepStrictEqual(instances[0].id, 'foo');
      assert.deepStrictEqual(instances[1].id, 'bar');
    }
  },

  async testRestartNoOptions(_, env) {
    const instance = await env.workflow.get('restart-basic');
    await instance.restart();

    const body = await getLastRestartBody(env, 'restart-basic');
    assert.deepStrictEqual(body.id, 'restart-basic');
    assert.strictEqual(body.from, undefined);
  },

  async testRestartFromStepNameOnly(_, env) {
    const instance = await env.workflow.get('restart-step');
    await instance.restart({ from: { name: 'fetch data' } });

    const body = await getLastRestartBody(env, 'restart-step');
    assert.deepStrictEqual(body.id, 'restart-step');
    assert.deepStrictEqual(body.from, { name: 'fetch data' });
  },

  async testRestartFromStepAllOptions(_, env) {
    const instance = await env.workflow.get('restart-full');
    await instance.restart({
      from: { name: 'process item', count: 3, type: 'do' },
    });

    const body = await getLastRestartBody(env, 'restart-full');
    assert.deepStrictEqual(body.id, 'restart-full');
    assert.deepStrictEqual(body.from, {
      name: 'process item',
      count: 3,
      type: 'do',
    });
  },
};
