// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

async function lastRestartBody(env, id) {
  return env.mock.lastRestart(id);
}

export const tests = {
  async test(_, env) {
    {
      const instance = await env.workflow.create({
        id: 'foo',
        payload: { bar: 'baz' },
      });
      assert.deepStrictEqual(instance.id, 'foo');
    }

    {
      const instance = await env.workflow.get('bar');
      assert.deepStrictEqual(instance.id, 'bar');
    }

    {
      const instances = await env.workflow.createBatch([
        { id: 'foo', payload: { bar: 'baz' } },
        { id: 'bar', payload: { bar: 'baz' } },
      ]);
      assert.deepStrictEqual(instances[0].id, 'foo');
      assert.deepStrictEqual(instances[1].id, 'bar');
    }

    {
      const instance = await env.workflow.get('inst');
      await instance.pause();
      await instance.resume();
      await instance.terminate();
      await instance.sendEvent({
        type: 'my-event',
        payload: { hello: 'world' },
      });
    }

    {
      const instance = await env.workflow.get('status-rpc');
      const status = await instance.status();
      assert.deepStrictEqual(status.status, 'running');
      assert.strictEqual(status.transport, 'rpc');
      assert.strictEqual(status.output, 'status-rpc');
    }

    {
      for (const method of ['get', 'create', 'createBatch']) {
        assert.strictEqual(typeof env.workflow[method], 'function');
      }

      const fromGet = await env.workflow.get('a');
      const fromCreate = await env.workflow.create({ id: 'b' });
      const [fromBatch] = await env.workflow.createBatch([{ id: 'c' }]);

      const proto = Object.getPrototypeOf(fromGet);
      assert.strictEqual(Object.getPrototypeOf(fromCreate), proto);
      assert.strictEqual(Object.getPrototypeOf(fromBatch), proto);

      for (const method of [
        'pause',
        'resume',
        'terminate',
        'restart',
        'status',
        'sendEvent',
      ]) {
        assert.strictEqual(typeof fromGet[method], 'function');
      }
    }

    {
      await assert.rejects(env.workflow.get('throw'), {
        message: 'workflow instance not found',
      });
    }
  },

  async testRestartNoOptions(_, env) {
    const instance = await env.workflow.get('rpc-restart-basic');
    await instance.restart();

    const body = await lastRestartBody(env, 'rpc-restart-basic');
    assert.deepStrictEqual(body.id, 'rpc-restart-basic');
    assert.strictEqual(body.from, undefined);
  },

  async testRestartFromStepNameOnly(_, env) {
    const instance = await env.workflow.get('rpc-restart-step');
    await instance.restart({ from: { name: 'fetch data' } });

    const body = await lastRestartBody(env, 'rpc-restart-step');
    assert.deepStrictEqual(body.id, 'rpc-restart-step');
    assert.deepStrictEqual(body.from, { name: 'fetch data' });
  },

  async testRestartFromStepAllOptions(_, env) {
    const instance = await env.workflow.get('rpc-restart-full');
    await instance.restart({
      from: { name: 'process item', count: 3, type: 'do' },
    });

    const body = await lastRestartBody(env, 'rpc-restart-full');
    assert.deepStrictEqual(body.id, 'rpc-restart-full');
    assert.deepStrictEqual(body.from, {
      name: 'process item',
      count: 3,
      type: 'do',
    });
  },
};
