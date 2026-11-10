// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { WorkflowEntrypoint } from 'cloudflare:workers';
import { NonRetryableError } from 'cloudflare:workflows';
import assert from 'node:assert';

// Each step callback and rollback that ran, as `<instance>/<what>`.
const ran = [];
const ranBy = (id) => ran.filter((r) => r.startsWith(`${id}/`));

// Promises a test and a Flow share, by name.
const signals = new Map();
function signal(name) {
  if (!signals.has(name)) {
    let resolve;
    const promise = new Promise((r) => (resolve = r));
    signals.set(name, { promise, resolve });
  }
  return signals.get(name);
}

// What an instance does is its `kind`.
export class Flow extends WorkflowEntrypoint {
  async run(event, step) {
    const id = event.instanceId;
    const once = (name, value = name) =>
      step.do(name, async () => {
        ran.push(`${id}/${name}`);
        return value;
      });
    switch (event.payload.kind) {
      case 'steps':
        return (await once('a', 1)) + (await once('b', 2));
      case 'backoff': {
        const delay = ({ ctx }) => ctx.attempt * 2000;
        const retries = { limit: 1, delay };
        return step.do('flaky', { retries }, async (ctx) => {
          if (ctx.attempt < 2) throw new Error('fail');
          return ctx.attempt;
        });
      }
      case 'slow': {
        const config = { retries: { limit: 0, delay: 0 }, timeout: '1 minute' };
        return step.do('slow', config, () => signal(`${id}/slow`).promise);
      }
      case 'hold': {
        const held = () => signal(`${id}/held`).promise;
        const value = await step.do('held', held).catch((e) => e.message);
        signal(`${id}/ended`).resolve(value);
        return value;
      }
      case 'retry': {
        const retries = { limit: 2, delay: '1 second', backoff: 'constant' };
        return step.do('flaky', { retries }, async (ctx) => {
          ran.push(`${id}/flaky${ctx.attempt}`);
          if (ctx.attempt < 3) throw new Error(`fail ${ctx.attempt}`);
          return ctx.attempt;
        });
      }
      case 'sleep':
        await once('a');
        await step.sleep('nap', '1 hour');
        return once('b');
      case 'event': {
        const options = { type: 'ping', timeout: '1 minute' };
        return (await step.waitForEvent('ping', options)).payload;
      }
      case 'fatal':
        return step.do('boom', async () => {
          ran.push(`${id}/boom`);
          throw new NonRetryableError('no');
        });
      case 'rollback': {
        const rollback = async ({ output }) => ran.push(`${id}/undo ${output}`);
        await step.do('a', async () => 'A', { rollback });
        return step.do('b', async () => {
          throw new NonRetryableError('b failed');
        });
      }
      case 'undo': {
        const rollback = async ({ output }) => ran.push(`${id}/undo ${output}`);
        await step.do('a', async () => 'A', { rollback });
        await step.sleep('nap', '1 hour');
        return once('b');
      }
    }
  }
}

// The n-th event of `type` in the instance's history, once it happens.
async function until(instance, type, n = 1) {
  const subscription = await instance.subscribe();
  for (;;) {
    const { done, value } = await subscription.next();
    if (done) throw new Error(`ended before ${n} x ${type}`);
    if (value.type === type && --n === 0) return value;
  }
}

// Moves the engine's clock; the instance acts on it at its next call.
async function advance(env, instance, ms) {
  await env.WORKFLOWS.fetch(`http://w/clock?advance=${ms}`, { method: 'POST' });
  return instance.status();
}

const create = (env, id, kind, options = {}) =>
  env.FLOW.create({ id, params: { kind }, ...options });

export let steps = {
  async test(ctrl, env) {
    const instance = await create(env, 'steps', 'steps');
    await until(instance, 'workflow_completed');
    const { status, output } = await instance.status();
    assert.deepEqual([status, output], ['complete', 3]);
    const filter = ['step_completed'];
    const subscription = await instance.subscribe({ cursor: 2, filter });
    const seen = [];
    for (;;) {
      const { done, value } = await subscription.next();
      if (done) break;
      seen.push(value);
    }
    assert.deepEqual(
      seen.map((e) => e.output),
      [1, 2]
    );
    assert.ok(seen.every((e) => e.eventId > 2));
    // Restarted from "b": "a" returns its stored result.
    await instance.restart({ from: { name: 'b' } });
    await until(instance, 'workflow_completed');
    await instance.restart();
    await until(instance, 'workflow_completed');
    assert.deepEqual(ranBy('steps'), [
      'steps/a',
      'steps/b',
      'steps/b',
      'steps/a',
      'steps/b',
    ]);
    await assert.rejects(
      instance.restart({ from: { name: 'c' } }),
      /not found/
    );
  },
};

export let retries = {
  async test(ctrl, env) {
    const instance = await create(env, 'retry', 'retry');
    for (const n of [1, 2]) {
      const e = await until(instance, 'attempt_errored', n);
      assert.deepEqual([e.retryDelayMs, e.error.message], [1000, `fail ${n}`]);
      await advance(env, instance, 1000);
    }
    await until(instance, 'workflow_completed');
    assert.strictEqual((await instance.status()).output, 3);
    assert.deepEqual(ranBy('retry'), [
      'retry/flaky1',
      'retry/flaky2',
      'retry/flaky3',
    ]);
  },
};

export let delays = {
  async test(ctrl, env) {
    const backoff = await create(env, 'backoff', 'backoff');
    const e = await until(backoff, 'attempt_errored');
    assert.strictEqual(e.retryDelayMs, 2000);
    await advance(env, backoff, 2000);
    await until(backoff, 'workflow_completed');
    assert.strictEqual((await backoff.status()).output, 2);

    const slow = await create(env, 'slow', 'slow');
    await until(slow, 'attempt_started');
    await advance(env, slow, 60 * 1000);
    const { error } = await until(slow, 'workflow_errored');
    assert.match(error.message, /timed out after 60000ms/);
  },
};

// A step of a deleted instance does not reach the one created in its place.
export let recreate = {
  async test(ctrl, env) {
    await until(await create(env, 'held', 'hold'), 'attempt_started');
    await env.FLOW.deleteBatch(['held']);
    const instance = await create(env, 'held', 'event');
    await until(instance, 'wait_started');
    signal('held/held').resolve('old');
    assert.match(await signal('held/ended').promise, /finished or restarted/);
    await instance.sendEvent({ type: 'ping', payload: 'new' });
    await until(instance, 'workflow_completed');
    assert.strictEqual((await instance.status()).output, 'new');
  },
};

// A pause stops the sleep's clock; terminate ends it.
export let sleepPauseResumeTerminate = {
  async test(ctrl, env) {
    const instance = await create(env, 'sleep', 'sleep');
    await until(instance, 'sleep_started');
    await instance.pause();
    assert.strictEqual(
      (await advance(env, instance, 60 * 60 * 1000)).status,
      'paused'
    );
    await instance.resume();
    assert.strictEqual((await instance.status()).status, 'waiting');
    await advance(env, instance, 60 * 60 * 1000);
    await until(instance, 'workflow_completed');
    assert.deepEqual(ranBy('sleep'), ['sleep/a', 'sleep/b']);

    const other = await create(env, 'sleep2', 'sleep');
    await until(other, 'sleep_started');
    await other.terminate();
    assert.strictEqual((await other.status()).status, 'terminated');
    await assert.rejects(other.terminate(), /cannot_terminate/);
    await assert.rejects(other.resume(), /cannot_resume/);
  },
};

export let events = {
  async test(ctrl, env) {
    const instance = await create(env, 'event', 'event');
    await until(instance, 'wait_started');
    await instance.sendEvent({ type: 'other', payload: 0 });
    await instance.sendEvent({ type: 'ping', payload: 'pong' });
    await until(instance, 'workflow_completed');
    assert.strictEqual((await instance.status()).output, 'pong');

    // Sent before the instance waits for it.
    const early = await create(env, 'early', 'event');
    await early.sendEvent({ type: 'ping', payload: 'first' });
    await until(early, 'workflow_completed');
    assert.strictEqual((await early.status()).output, 'first');

    const late = await create(env, 'late', 'event');
    await until(late, 'wait_started');
    await advance(env, late, 60 * 1000);
    await until(late, 'workflow_errored');
    assert.match(
      (await late.status()).error.message,
      /timed out after 60000ms/
    );
  },
};

export let failures = {
  async test(ctrl, env) {
    const fatal = await create(env, 'fatal', 'fatal');
    const e = await until(fatal, 'workflow_errored');
    assert.match(e.error.message, /no/);
    assert.deepEqual(ranBy('fatal'), ['fatal/boom']);

    const rollback = await create(env, 'rollback', 'rollback');
    await until(rollback, 'workflow_errored');
    await until(rollback, 'rollback_completed');
    assert.deepEqual(ranBy('rollback'), ['rollback/undo A']);

    // Terminated with its rollbacks, the sleep never ends.
    const undo = await create(env, 'undo', 'undo');
    await until(undo, 'sleep_started');
    await undo.terminate({ rollback: true });
    assert.strictEqual((await undo.status()).status, 'terminated');
    await until(undo, 'rollback_completed');
    await advance(env, undo, 60 * 60 * 1000);
    assert.deepEqual(ranBy('undo'), ['undo/undo A']);
  },
};

export let api = {
  async test(ctrl, env) {
    await assert.rejects(env.FLOW.get('missing'), /instance.not_found/);
    await assert.rejects(env.FLOW.create({ id: 'a b' }), /invalid id/);
    await create(env, 'one', 'steps');
    await assert.rejects(create(env, 'one', 'steps'), /already_exists/);
    const { created, errors } = await env.FLOW.createBatch({
      instances: [{ id: 'b1' }, { id: 'b1' }, { id: 'one' }],
    });
    assert.deepEqual(
      created.map((i) => i.id),
      ['b1']
    );
    assert.deepEqual(
      errors.map((e) => [e.index, e.code]),
      [
        [1, 10415],
        [2, 10405],
      ]
    );
    const deleted = await env.FLOW.deleteBatch(['b1', 'nope']);
    assert.deepEqual(deleted.deleted, [{ id: 'b1' }]);
    assert.deepEqual(
      deleted.errors.map((e) => [e.id, e.code]),
      [['nope', 10400]]
    );
    await assert.rejects(env.FLOW.get('b1'), /instance.not_found/);
  },
};

// A finished instance is deleted after its retention.
export let retention = {
  async test(ctrl, env) {
    const successRetention = '1 day';
    const instance = await create(env, 'kept', 'steps', {
      retention: { successRetention },
    });
    await until(instance, 'workflow_completed');
    await assert.rejects(
      advance(env, instance, 24 * 60 * 60 * 1000),
      /instance.not_found/
    );
  },
};
