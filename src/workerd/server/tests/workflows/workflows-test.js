// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { WorkflowEntrypoint } from 'cloudflare:workers';
import { NonRetryableError } from 'cloudflare:workflows';
import assert from 'node:assert';

// Each step callback and rollback that ran, as `<instance>/<what>`.
const ran = [];
const ranBy = (id) => ran.filter((r) => r.startsWith(`${id}/`));

const branched = new Set();

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
      case 'many':
        for (const name of ['a', 'b', 'c', 'd']) await once(name);
        return;
      case 'self-delete':
        await once('a');
        await (await this.env.FLOW.get(id)).delete();
        ran.push(`${id}/after`);
        return;
      case 'big': {
        const retries = { limit: 1, delay: 0 };
        return step.do('big', { retries }, async (ctx) =>
          ctx.attempt === 1 ? 'x'.repeat(9 * 1024 * 1024) : 'ok'
        );
      }
      case 'stream-text':
      case 'stream-binary':
      case 'stream-error':
      case 'stream-hang': {
        const kind = event.payload.kind;
        const chunks = {
          'stream-text': ['a'.repeat(300 * 1024), 'end'],
          'stream-binary': [new Uint8Array([0xff, 0xfe])],
          'stream-hang': [new Uint8Array([1])],
        }[kind];
        const encode = (c) =>
          typeof c === 'string' ? new TextEncoder().encode(c) : c;
        const retries = { limit: 0, delay: 0 };
        const config = { retries, timeout: '1 second' };
        const s = await step.do('s', config, async () => {
          ran.push(`${id}/s`);
          return new ReadableStream({
            start(c) {
              if (kind === 'stream-error') return c.error(new Error('broke'));
              for (const chunk of chunks) c.enqueue(encode(chunk));
              if (kind !== 'stream-hang') c.close();
            },
          });
        });
        const text = await new Response(s).text();
        await step.sleep('nap', '1 hour');
        return `${text.length}:${text.slice(-3)}`;
      }
      case 'void':
        return step.do('v', async () => {});
      case 'dispose':
      case 'late-result': {
        // The disposer tells the instance, which waits for it.
        const tell = async () =>
          (await this.env.FLOW.get(id)).sendEvent({ type: 'disposed' });
        const result = { [Symbol.dispose]: () => this.ctx.waitUntil(tell()) };
        if (event.payload.kind === 'dispose') {
          await step.do('d', async () => result);
        } else {
          // Its only attempt times out; the result comes after.
          const late = async () => {
            await until(await this.env.FLOW.get(id), 'step_errored');
            return result;
          };
          const retries = { limit: 0, delay: 0 };
          const config = { retries, timeout: '1 second' };
          await step.do('late', config, late).catch(() => {});
        }
        const wait = { type: 'disposed', timeout: '30 seconds' };
        return step.waitForEvent('disposed', wait);
      }
      case 'backoff': {
        const delay = ({ ctx }) => ctx.attempt * 2000;
        const retries = { limit: 1, delay };
        return step.do('flaky', { retries }, async (ctx) => {
          if (ctx.attempt < 2) throw new Error('fail');
          return ctx.attempt;
        });
      }
      case 'delay-hang':
      case 'delay-bad':
      case 'delay-throw': {
        const delay = {
          'delay-hang': () => new Promise(() => {}),
          'delay-bad': () => 'soon',
          'delay-throw': () => {
            throw new Error('nope');
          },
        }[event.payload.kind];
        return step.do('flaky', { retries: { limit: 1, delay } }, () => {
          throw new Error('fail');
        });
      }
      case 'bad-config': {
        const retries = { limit: 1, delay: '1 second', trains: true };
        return step.do('x', { retries }, async () => 1);
      }
      case 'slow': {
        const config = { retries: { limit: 0, delay: 0 }, timeout: '1 minute' };
        return step.do('slow', config, () => signal(`${id}/slow`).promise);
      }
      case 'hold':
        return step.do('held', () => signal(`${id}/held`).promise);
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
      case 'rest': {
        await once('a');
        await step.sleep('nap', '1 hour').catch((e) => {
          signal(`${id}/rested`).resolve(e.message);
          throw e;
        });
        return once('b');
      }
      case 'event':
      case 'long-event': {
        const long = event.payload.kind === 'long-event';
        const options = { type: 'ping', timeout: long ? '1 hour' : '1 minute' };
        return (await step.waitForEvent('ping', options)).payload;
      }
      case 'fatal':
        return step.do('boom', async () => {
          ran.push(`${id}/boom`);
          throw new NonRetryableError('no');
        });
      case 'caught': {
        const boom = step.do('boom', async () => {
          ran.push(`${id}/boom`);
          throw new NonRetryableError('no');
        });
        await boom.catch(() => {});
        return once('after');
      }
      case 'rollback': {
        const rollback = async ({ output }) => ran.push(`${id}/undo ${output}`);
        await step.do('a', async () => 'A', { rollback });
        return step.do('b', async () => {
          throw new NonRetryableError('b failed');
        });
      }
      case 'branchy': {
        // Only the first run registers the rollback.
        const first = !branched.has(id);
        branched.add(id);
        const rollback = async () => ran.push(`${id}/undo`);
        await step.do('setup', async () => 1, first ? { rollback } : {});
        return step.sleep('nap', '1 hour');
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

// The finished instance's history: `[type, stepName, ...]` of the events of
// `types`.
async function history(instance, types) {
  const subscription = await instance.subscribe({ filter: types });
  const events = [];
  for (;;) {
    const { done, value } = await subscription.next();
    if (done) return events;
    const { type, stepName, attempt, error } = value;
    events.push([type, stepName, attempt ?? error?.name].filter(Boolean));
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

// An output too large to store fails the attempt, which is retried.
export let bigOutput = {
  async test(ctrl, env) {
    const instance = await create(env, 'big', 'big');
    const { error } = await until(instance, 'attempt_errored');
    assert.deepEqual(error, {
      name: 'WorkflowInternalError',
      message: 'Step big-1 output is too large. Maximum allowed size is 1MiB.',
    });
    await until(instance, 'workflow_completed');
    assert.strictEqual((await instance.status()).output, 'ok');
  },
};

// A step's stream is stored, shown in the logs, and replayed from storage.
export let streams = {
  async test(ctrl, env) {
    const text = await create(env, 'stream', 'stream-text');
    const { output } = await until(text, 'step_completed');
    assert.strictEqual(output, 'a'.repeat(1024) + '[truncated output]');
    await until(text, 'workflow_waiting');
    await advance(env, text, 60 * 60 * 1000);
    await until(text, 'workflow_completed');
    assert.strictEqual((await text.status()).output, `${300 * 1024 + 3}:end`);
    assert.deepEqual(ranBy('stream'), ['stream/s']);

    const binary = await create(env, 'binary', 'stream-binary');
    const shown = await until(binary, 'step_completed');
    assert.strictEqual(shown.output, '[ReadableStream (binary): 2 bytes]');

    const broken = await create(env, 'broken', 'stream-error');
    const { error } = await until(broken, 'workflow_errored');
    assert.match(error.message, /returned an invalid ReadableStream output/);

    const hang = await create(env, 'hang', 'stream-hang');
    await until(hang, 'attempt_started');
    await advance(env, hang, 1000);
    const errored = await until(hang, 'attempt_errored');
    assert.strictEqual(errored.error.name, 'WorkflowTimeoutError');
  },
};

export let voidStep = {
  async test(ctrl, env) {
    const instance = await create(env, 'void', 'void');
    const event = await until(instance, 'step_completed');
    assert.ok(!('output' in event));
  },
};

// A step's result is released once stored, and a timed-out attempt's
// result when it comes.
// A step's result is released once stored, and a timed-out attempt's
// result when it comes.
export let disposeResults = {
  async test(ctrl, env) {
    const instance = await create(env, 'dispose', 'dispose');
    await until(instance, 'workflow_completed');
    const late = await create(env, 'late-result', 'late-result');
    await until(late, 'attempt_started');
    await advance(env, late, 1000);
    await until(late, 'workflow_completed');
  },
};

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
    // Its history keeps the start and "a".
    const kept = ['workflow_queued', 'workflow_started', 'step_completed'];
    assert.deepEqual(await history(instance, kept), [
      ['workflow_queued'],
      ['workflow_started'],
      ['step_completed', 'a-1'],
      ['step_completed', 'b-1'],
    ]);
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

    // A delay function that fails fails the step: it runs no more.
    const failing = [
      ['delay-hang', 'did not return within 5 seconds'],
      ['delay-bad', 'returned an invalid delay value'],
      ['delay-throw', 'threw an error: nope'],
    ];
    for (const [kind, message] of failing) {
      const instance = await create(env, kind, kind);
      if (kind === 'delay-hang') {
        await until(instance, 'workflow_waiting');
        await advance(env, instance, 5000);
      }
      const { error } = await until(instance, 'workflow_errored');
      assert.match(error.message, new RegExp(message));
      const steps = ['attempt_errored', 'step_errored'];
      assert.deepEqual(await history(instance, steps), [
        ['attempt_errored', 'flaky-1', 1],
        ['step_errored', 'flaky-1'],
      ]);
    }

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
    await instance.sendEvent({ type: 'ping', payload: 'new' });
    await until(instance, 'workflow_completed');
    assert.strictEqual((await instance.status()).output, 'new');
  },
};

// An instance that deletes itself stops there.
export let selfDelete = {
  async test(ctrl, env) {
    await assert.rejects(until(await create(env, 'self', 'self-delete'), '-'));
    await assert.rejects(env.FLOW.get('self'), /not_found/);
    // By its end, code after the delete would have run.
    await until(await create(env, 'self', 'steps'), 'workflow_completed');
    assert.deepEqual(ranBy('self'), ['self/a', 'self/a', 'self/b']);
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

// A long sleep ends `run()` in memory; it runs again at the deadline.
export let rest = {
  async test(ctrl, env) {
    const instance = await create(env, 'rest', 'rest');
    await until(instance, 'workflow_waiting');
    assert.strictEqual((await instance.status()).status, 'waiting');
    await advance(env, instance, 60 * 60 * 1000);
    await until(instance, 'workflow_completed');
    assert.deepEqual(ranBy('rest'), ['rest/a', 'rest/b']);
    assert.match(
      await signal('rest/rested').promise,
      /run of the instance has ended/
    );
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

    // Sent while the instance rests.
    const rested = await create(env, 'rested', 'long-event');
    await until(rested, 'workflow_waiting');
    await rested.sendEvent({ type: 'ping', payload: 'woke' });
    await until(rested, 'workflow_completed');
    assert.strictEqual((await rested.status()).output, 'woke');

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
    // Reported as thrown under workflows_preserve_non_retryable_error_message.
    const preserve =
      Cloudflare.compatibilityFlags
        .workflows_preserve_non_retryable_error_message;
    const { error: attempt } = await until(fatal, 'attempt_errored');
    const { error: e } = await until(fatal, 'workflow_errored');
    assert.deepEqual(
      [attempt, e],
      preserve
        ? [
            { name: 'NonRetryableError', message: 'no' },
            { name: 'NonRetryableError', message: 'no' },
          ]
        : [
            {
              name: 'WorkflowFatalError',
              message: 'Step threw a NonRetryableError with message "no"',
            },
            {
              name: 'WorkflowFatalError',
              message:
                'The execution of the Workflow instance was terminated, as ' +
                'a step threw an NonRetryableError and it was not handled',
            },
          ]
    );

    // The Workflow's step limit is 3.
    const many = await create(env, 'many', 'many');
    const { error } = await until(many, 'workflow_errored');
    assert.match(error.message, /limit of 3 steps/);
    assert.deepEqual(ranBy('many'), ['many/a', 'many/b', 'many/c']);
    assert.deepEqual(ranBy('fatal'), ['fatal/boom']);

    // Replayed, a caught NonRetryableError is rethrown without running again.
    const caught = await create(env, 'caught', 'caught');
    await until(caught, 'workflow_completed');
    await caught.restart({ from: { name: 'after' } });
    await until(caught, 'workflow_completed');
    assert.deepEqual(ranBy('caught'), [
      'caught/boom',
      'caught/after',
      'caught/after',
    ]);

    const rollback = await create(env, 'rollback', 'rollback');
    await until(rollback, 'workflow_errored');
    await until(rollback, 'rollback_completed');
    assert.deepEqual(ranBy('rollback'), ['rollback/undo A']);
    const rollbacks = [
      'rollback_step_started',
      'rollback_attempt_started',
      'rollback_attempt_completed',
      'rollback_step_completed',
      'rollback_completed',
    ];
    assert.deepEqual(await history(rollback, rollbacks), [
      ['rollback_step_started', 'a-1'],
      ['rollback_attempt_started', 'a-1', 1],
      ['rollback_attempt_completed', 'a-1', 1],
      ['rollback_step_completed', 'a-1'],
      ['rollback_completed'],
    ]);

    // A rollback that replay does not register again is missing.
    const branchy = await create(env, 'branchy', 'branchy');
    await until(branchy, 'workflow_waiting');
    await branchy.terminate({ rollback: true });
    const errored = ['rollback_step_errored', 'rollback_errored'];
    assert.deepEqual(await history(branchy, errored), [
      ['rollback_step_errored', 'setup-1', 'RollbackMissing'],
      ['rollback_errored'],
    ]);
    assert.deepEqual(ranBy('branchy'), []);

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
    const retention = { successRetention: 'soon' };
    await assert.rejects(
      env.FLOW.createBatch({ instances: [{ id: 'b2', retention }] }),
      /Duration must be a number/
    );
    const deleted = await env.FLOW.deleteBatch(['b1', 'nope']);
    assert.deepEqual(deleted.deleted, [{ id: 'b1' }]);
    assert.deepEqual(
      deleted.errors.map((e) => [e.id, e.code]),
      [['nope', 10400]]
    );
    await assert.rejects(env.FLOW.get('b1'), /instance.not_found/);
    // Every entry of a batch is checked before any is created.
    const invalid = [
      [{ instances: [[]] }, /expected object, received array/],
      [{ instances: [{ locationHint: 'mars' }] }, /expected one of "wnam"/],
      ...[-1, 0, 0.5, '0 days', '-1 day'].map((successRetention) => [
        { instances: [{ retention: { successRetention } }] },
        /Duration must be a number/,
      ]),
    ];
    for (const [options, message] of invalid) {
      await assert.rejects(env.FLOW.createBatch(options), message);
    }
    await assert.rejects(
      env.FLOW.createBatch([{ id: 'good' }, { id: '#invalid!' }]),
      /Workflow instance has invalid id/
    );
    await assert.rejects(env.FLOW.get('good'), /instance.not_found/);
    // The array form ignores retention.
    const loose = { id: 'loose', params: { kind: 'steps' } };
    const soon = { successRetention: 'soon' };
    await env.FLOW.createBatch([{ ...loose, retention: soon }]);

    const config = await create(env, 'bad-config', 'bad-config');
    const { error } = await until(config, 'workflow_errored');
    assert.match(error.message, /Step config for "x" is in a invalid format/);

    for (const options of [null, [], { filter: ['internal'] }]) {
      await assert.rejects(
        config.subscribe(options),
        /Invalid Workflow subscription options/
      );
    }
    const big = { cursor: Number.MAX_SAFE_INTEGER + 1 };
    await assert.rejects(config.subscribe(big), /subscription options/);

    const clock = 'http://w/clock?advance=soon';
    const r = await env.WORKFLOWS.fetch(clock, { method: 'POST' });
    assert.strictEqual(r.status, 400);
  },
};

// A finished instance is deleted after its retention: a terminated one
// after its success retention, and one that sets none after the Workflow's
// (2 days).
export let retention = {
  async test(ctrl, env) {
    const day = 24 * 60 * 60 * 1000;
    const retention = { successRetention: '1 day' };
    const kept = await create(env, 'kept', 'steps', { retention });
    await until(kept, 'workflow_completed');
    const ended = await create(env, 'ended', 'sleep', { retention });
    await until(ended, 'sleep_started');
    await ended.terminate();
    const defaulted = await create(env, 'defaulted', 'steps');
    await until(defaulted, 'workflow_completed');
    await assert.rejects(advance(env, kept, day), /instance.not_found/);
    await assert.rejects(ended.status(), /instance.not_found/);
    assert.strictEqual((await defaulted.status()).status, 'complete');
    await assert.rejects(advance(env, defaulted, day), /instance.not_found/);
  },
};
