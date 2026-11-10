// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { DurableObject } from 'cloudflare:workers';
import assert from 'node:assert';

// Every delivery, in order.
export class Seen extends DurableObject {
  log = [];
  #waiters = [];
  add(entries) {
    this.log.push(...entries);
    for (const wake of this.#waiters.splice(0)) wake();
  }
  entries() {
    return this.log;
  }
  #held;
  // Resolves once `release()` is called.
  hold() {
    return new Promise((wake) => (this.#held = wake));
  }
  release() {
    this.#held();
  }
  // The log, once it holds `n` entries.
  async until(n) {
    while (this.log.length < n) {
      await new Promise((wake) => this.#waiters.push(wake));
    }
    return this.log;
  }
}

const seen = (ctx) => ctx.exports.Seen.getByName('seen');
const until = (ctx, n) => seen(ctx).until(n);

export default {
  async queue(batch, env, ctx) {
    // Large bodies are logged by size and retried on "jobs".
    const big = (b) => b instanceof ArrayBuffer && b.byteLength > 16;
    const entries = batch.messages.map((m) => ({
      queue: batch.queue,
      body: big(m.body)
        ? { bytes: m.body.byteLength }
        : m.body instanceof ArrayBuffer
          ? [...new Uint8Array(m.body)]
          : m.body,
      attempts: m.attempts,
      size: batch.messages.length,
    }));
    await seen(ctx).add(entries);
    if (batch.messages.some((m) => m.body === 'hang' && m.attempts === 1))
      await seen(ctx).hold();
    for (const m of batch.messages) {
      if ((m.body === 'fail' || big(m.body)) && batch.queue === 'jobs')
        m.retry();
      else if (m.body === 'throw') m.ack();
      else if (m.body === 'again' && m.attempts === 1) m.retry();
    }
    if (batch.messages.some((m) => m.body === 'throw')) throw new Error('boom');
  },
};

export let formats = {
  async test(ctrl, env, ctx) {
    const before = (await seen(ctx).entries()).length;
    await env.JOBS.send('text', { contentType: 'text' });
    await env.JOBS.send({ json: 1 }, { contentType: 'json' });
    await env.JOBS.send(new Uint8Array([1, 2]), { contentType: 'bytes' });
    await env.JOBS.send(new Map([[1, 2]]), { contentType: 'v8' });
    const log = (await until(ctx, before + 4)).slice(before);
    assert.deepEqual(
      log.map((e) => e.body),
      ['text', { json: 1 }, [1, 2], new Map([[1, 2]])]
    );
    assert(log.every((e) => e.attempts === 1 && e.queue === 'jobs'));
  },
};

export let batchesRetriesAndDeadLetters = {
  async test(ctrl, env, ctx) {
    const before = (await seen(ctx).entries()).length;
    // One batch of four: a retry goes to "dead" after its second attempt;
    // an explicit ack wins over the exception that retries the rest.
    await env.JOBS.sendBatch([
      { body: 'fail' },
      { body: 'throw' },
      { body: 'ok' },
      { body: 'late', delaySeconds: 1 },
    ]);
    // All but "late", which is due once the clock moves a second.
    const early = (await until(ctx, before + 6)).slice(before);
    assert(!early.some((e) => e.body === 'late'));
    await env.QUEUES.fetch('http://q/clock?advance=1000', { method: 'POST' });
    await env.JOBS.metrics();
    const log = (await until(ctx, before + 7)).slice(before);
    const of = (body) =>
      log.filter((e) => e.body === body).map((e) => [e.queue, e.attempts]);
    assert.deepEqual(of('fail'), [
      ['jobs', 1],
      ['jobs', 2],
      ['dead', 1],
    ]);
    assert.deepEqual(of('throw'), [['jobs', 1]]);
    // Retried by the exception, then acked by the consumer returning.
    assert.deepEqual(of('ok'), [
      ['jobs', 1],
      ['jobs', 2],
    ]);
    assert.deepEqual(of('late'), [['jobs', 1]]);
  },
};

// A delivery outliving its lease is a failed attempt, and no longer holds
// "serial"'s one batch in flight: the batch goes out again.
export let leaseExpiry = {
  async test(ctrl, env, ctx) {
    const before = (await seen(ctx).entries()).length;
    const hang = { body: 'hang', contentType: 'text' };
    await env.SERIAL.sendBatch([hang, hang]);
    await until(ctx, before + 2);
    const minutes = 16 * 60 * 1000;
    await env.QUEUES.fetch(`http://q/clock?advance=${minutes}`, {
      method: 'POST',
    });
    await env.SERIAL.metrics();
    const log = (await until(ctx, before + 4)).slice(before);
    assert.deepEqual(
      log.map((e) => e.attempts),
      [1, 1, 2, 2]
    );
    await seen(ctx).release();
  },
};

// A retried message goes out again once it is visible, without waiting
// for the batch timeout (60 s on "serial") a second time.
export let retriesSkipTheTimeout = {
  async test(ctrl, env, ctx) {
    const before = (await seen(ctx).entries()).length;
    await env.SERIAL.send('again', { contentType: 'text' });
    await env.QUEUES.fetch('http://q/clock?advance=60000', { method: 'POST' });
    await env.SERIAL.metrics();
    const log = (await until(ctx, before + 2)).slice(before);
    assert.deepEqual(
      log.map((e) => e.attempts),
      [1, 2]
    );
  },
};

// Full batches go out one after another, each as the last one settles,
// not on the batch timeout (60 s).
export let fullBatchesInTurn = {
  async test(ctrl, env, ctx) {
    const before = (await seen(ctx).entries()).length;
    const body = (i) => ({ body: `s${i}`, contentType: 'text' });
    await env.SERIAL.sendBatch(Array.from({ length: 6 }, (_, i) => body(i)));
    const stalled = new Promise((_, reject) =>
      setTimeout(() => reject(new Error('stalled')), 10_000)
    );
    const log = (await Promise.race([until(ctx, before + 6), stalled])).slice(
      before
    );
    assert.deepEqual(
      log.map((e) => [e.body, e.size]),
      [0, 1, 2, 3, 4, 5].map((i) => [`s${i}`, 2])
    );
  },
};

// A message at production's size limit reaches the dead letter queue whole.
export let largeDeadLetter = {
  async test(ctrl, env, ctx) {
    const before = (await seen(ctx).entries()).length;
    await env.JOBS.send(new Uint8Array(128_000), { contentType: 'bytes' });
    const log = (await until(ctx, before + 3)).slice(before);
    assert.deepEqual(
      log.map((e) => [e.queue, e.attempts, e.body]),
      [
        ['jobs', 1, { bytes: 128_000 }],
        ['jobs', 2, { bytes: 128_000 }],
        ['dead', 1, { bytes: 128_000 }],
      ]
    );
  },
};

export let metrics = {
  async test(ctrl, env, ctx) {
    // No consumer: messages wait.
    await env.IDLE.send('a', { contentType: 'text' });
    await env.IDLE.send('bc', { contentType: 'text' });
    const m = await env.IDLE.metrics();
    assert.strictEqual(m.backlogCount, 2);
    assert.strictEqual(m.backlogBytes, 3);
    assert(m.oldestMessageTimestamp instanceof Date);
  },
};

// Production's limits; IDLE has no consumer, so nothing is delivered.
export let limits = {
  async test(ctrl, env) {
    const bytes = (n) => ({ body: new Uint8Array(n), contentType: 'bytes' });
    await env.BIG.send(new Uint8Array(128_000), { contentType: 'bytes' });
    await assert.rejects(
      env.BIG.send(new Uint8Array(128_001), { contentType: 'bytes' })
    );
    await env.BIG.sendBatch(Array.from({ length: 100 }, () => bytes(1)));
    await assert.rejects(
      env.BIG.sendBatch(Array.from({ length: 101 }, () => bytes(1)))
    );
    await assert.rejects(env.BIG.sendBatch([bytes(128_001)]));
    await env.BIG.sendBatch([bytes(128_000), bytes(128_000)]);
    await assert.rejects(
      env.BIG.sendBatch([bytes(128_000), bytes(128_000), bytes(1)])
    );
  },
};
