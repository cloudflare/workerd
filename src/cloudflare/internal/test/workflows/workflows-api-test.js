// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

// Every test is its own export: `workerd test` runs the `test()` handler of each entrypoint, so
// extra methods hung off a single exported object would silently never run.

// Introspection goes over `env.mock.fetch()` rather than JSRPC on purpose: `env.mock` is an
// ordinary service binding, so its RPC wildcard stays gated at this worker's compatibility date.
async function getLastRestartBody(env, id) {
  const res = await env.mock.fetch('http://placeholder/last-restart', {
    method: 'POST',
    body: JSON.stringify({ id }),
  });
  return (await res.json()).result;
}

async function getLastSubscribeOptions(env, id) {
  const res = await env.mock.fetch('http://placeholder/last-subscribe', {
    method: 'POST',
    body: JSON.stringify({ id }),
  });
  return (await res.json()).result;
}

export const workflowsApi = {
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

    {
      const result = await env.workflow.deleteBatch([
        'delete-1',
        'missing-delete',
        'delete-1',
      ]);
      assert.deepStrictEqual(result, {
        deleted: [{ id: 'delete-1' }, { id: 'delete-1' }],
        errors: [
          {
            id: 'missing-delete',
            code: 10400,
            message: 'workflows.api.error.instance.not_found',
          },
        ],
      });
    }

    {
      const instance = await env.workflow.get('inst');
      await instance.pause();
      await instance.resume();
      await instance.terminate();
      await instance.delete();
      await instance.sendEvent({
        type: 'my-event',
        payload: { hello: 'world' },
      });
    }

    {
      const instance = await env.workflow.get('status-1');
      const status = await instance.status();
      assert.deepStrictEqual(status.status, 'running');
      assert.strictEqual(status.output, 'status-1');
    }

    {
      for (const method of ['get', 'create', 'createBatch', 'deleteBatch']) {
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
        'delete',
        'status',
        'sendEvent',
        'subscribe',
      ]) {
        assert.strictEqual(typeof fromGet[method], 'function');
      }
    }

    {
      // The binding keeps its ungated inner fetcher inaccessible to user code.
      // Instances returned by the binding omit the fetcher too.
      assert.strictEqual(env.workflow.fetcher, undefined);
      assert.strictEqual((await env.workflow.get('d')).fetcher, undefined);
    }

    {
      await assert.rejects(env.workflow.get('throw'), {
        message: 'workflow instance not found',
      });
    }
  },
};

export const subscribeNoOptions = {
  async test(_, env) {
    const instance = await env.workflow.get('subscribe-basic');
    using subscription = await instance.subscribe();

    assert.deepStrictEqual(await subscription.next(), {
      done: false,
      value: {
        instanceId: 'subscribe-basic',
        eventId: 0,
        timestamp: 0,
        type: 'workflow_completed',
        output: 'done',
      },
    });
    assert.deepStrictEqual(await subscription.next(), {
      done: true,
      value: undefined,
    });
    assert.strictEqual(
      await getLastSubscribeOptions(env, 'subscribe-basic'),
      null
    );
  },
};

export const subscribeAllOptions = {
  async test(_, env) {
    const instance = await env.workflow.get('subscribe-full');
    using _subscription = await instance.subscribe({
      cursor: 1,
      filter: ['workflow_queued', 'workflow_completed'],
    });
    assert.deepStrictEqual(
      await getLastSubscribeOptions(env, 'subscribe-full'),
      { cursor: 1, filter: ['workflow_queued', 'workflow_completed'] }
    );
  },
};

export const restartNoOptions = {
  async test(_, env) {
    const instance = await env.workflow.get('restart-basic');
    await instance.restart();

    const body = await getLastRestartBody(env, 'restart-basic');
    assert.deepStrictEqual(body.id, 'restart-basic');
    assert.strictEqual(body.from, undefined);
  },
};

export const restartFromStepNameOnly = {
  async test(_, env) {
    const instance = await env.workflow.get('restart-step');
    await instance.restart({ from: { name: 'fetch data' } });

    const body = await getLastRestartBody(env, 'restart-step');
    assert.deepStrictEqual(body.id, 'restart-step');
    assert.deepStrictEqual(body.from, { name: 'fetch data' });
  },
};

export const restartFromStepAllOptions = {
  async test(_, env) {
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
