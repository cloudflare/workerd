// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';
import { DurableObject, tracing } from 'cloudflare:workers';
import { channel } from 'node:diagnostics_channel';

// A separate object controls the order without introducing another request to the probe.
export class Gates extends DurableObject {
  #gates = new Map();

  #gate(id) {
    if (!this.#gates.has(id)) {
      this.#gates.set(id, {
        entered: Promise.withResolvers(),
        released: Promise.withResolvers(),
      });
    }
    return this.#gates.get(id);
  }

  async wait(id) {
    this.#gate(id).entered.resolve();
    await this.#gate(id).released.promise;
  }
  async entered(id) {
    await this.#gate(id).entered.promise;
  }
  release(id) {
    this.#gate(id).released.resolve();
  }
}

const diagnostics = channel('do-log-attribution');
diagnostics.subscribe(() => {});

export class Probe extends DurableObject {
  #background = Promise.withResolvers();

  async fetch(request) {
    const id = new URL(request.url).pathname.slice(1);
    const emit = (phase) => console.log(JSON.stringify({ id, phase }));
    const control = this.env.GATES.get(this.env.GATES.idFromName('control'));
    emit('start');
    if (id === 'background/a') {
      // No I/O or RPC capability keeps A's tracer alive while this task waits.
      this.ctx.waitUntil(
        this.#background.promise.then(async () => {
          emit('resume');
          await tracing.enterSpan('background-child', async () => {
            await Promise.resolve();
            emit('nested');
            diagnostics.publish({ id });
          });
          emit('restored');
          await control.release('background/finished');
        })
      );
    } else {
      await control.wait('background/entered');
      this.#background.resolve();
      await control.wait('background/finished');
      emit('resume');
    }
    return new Response(id);
  }

  async run(id, fail) {
    const emit = (phase) => console.log(JSON.stringify({ id, phase }));
    emit('start');
    await this.env.GATES.get(this.env.GATES.idFromName('control')).wait(id);
    emit('resume');
    const span = tracing.startSpan('resumed-child');
    try {
      await tracing.enterSpan(span, async () => {
        await Promise.resolve();
        emit('nested');
        diagnostics.publish({ id });
      });
    } finally {
      span.end();
    }
    emit('restored');
    if (fail) {
      emit('failed');
      throw new Error('synthetic failure');
    }
    emit('finished');
    return id;
  }
}

export const test = {
  async test(ctrl, env) {
    const control = env.GATES.get(env.GATES.idFromName('control'));
    for (const mode of ['serial', 'a-first', 'a-fails', 'b-first']) {
      const stub = env.PROBE.get(env.PROBE.idFromName(mode));
      const aId = `${mode}/a`;
      const bId = `${mode}/b`;
      const start = (id, fail) =>
        stub.run(id, fail).then(
          (value) => ({ value }),
          (error) => ({ error: error.message })
        );
      const a = start(aId, mode === 'a-fails');
      await control.entered(aId);
      if (mode === 'serial') {
        await control.release(aId);
        await a;
      }
      const b = start(bId, false);
      await control.entered(bId);
      if (mode === 'b-first') {
        await control.release(bId);
        await b;
        await control.release(aId);
      } else {
        await control.release(aId);
        await a;
        await control.release(bId);
      }
      assert.deepEqual(
        await a,
        mode === 'a-fails' ? { error: 'synthetic failure' } : { value: aId }
      );
      assert.deepEqual(await b, { value: bId });
    }

    const background = env.PROBE.get(env.PROBE.idFromName('background'));
    const a = await background.fetch('http://example.com/background/a');
    assert.equal(await a.text(), 'background/a');
    const b = background.fetch('http://example.com/background/b');
    await control.entered('background/entered');
    // A's outcome proves its tracer has finished before its background task resumes.
    const ended = await env.TAIL.fetch('http://tail/background/a');
    assert.equal(await ended.text(), 'ended');
    await control.release('background/entered');
    assert.equal(await (await b).text(), 'background/b');
  },
};
