// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An EventSource stays alive while a read of its stream is pending, and is
// collectable once its stream has ended. An event that had a JavaScript
// wrapper before it was dispatched, as every event the TypeScript
// implementation dispatches has, keeps its target alive until the event
// itself is swept, so collection is checked over a few major GCs.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

async function collect(weakRef) {
  for (let i = 0; i < 3 && weakRef.deref() !== undefined; i++) {
    // Queue stackless GC before yielding so its task runs before this
    // request resumes.
    const collected = gc({ type: 'major', execution: 'async' });
    await scheduler.wait(0);
    await collected;
  }
}

async function checkLifetime(readable, write, end) {
  const received = Promise.withResolvers();
  const ended = Promise.withResolvers();
  const messages = [];
  let source = EventSource.from(readable);
  source.onmessage = (event) => {
    messages.push([event.data, event.lastEventId]);
    if (messages.length === 2) received.resolve();
  };
  source.onerror = () => ended.resolve();
  const weakSource = new WeakRef(source);
  source = null;

  // A pending read retains its EventSource.
  const pendingReadGc = gc({ type: 'major', execution: 'async' });
  await scheduler.wait(0);
  await pendingReadGc;
  ok(weakSource.deref());
  await scheduler.wait(0);
  await write('id: first\nretry: 1000\ndata: one\n\n');
  await write('id: second\ndata: two\n\n');
  await received.promise;
  deepStrictEqual(messages, [
    ['one', 'first'],
    ['two', 'second'],
  ]);
  await end();
  await ended.promise;

  // Settled reads leave no strong ownership cycle behind.
  await collect(weakSource);
  strictEqual(weakSource.deref(), undefined);
}

// A stream with a native source.
export const nativeStreamLifetime = {
  async test() {
    const stream = new IdentityTransformStream();
    const writer = stream.writable.getWriter();
    const encoder = new TextEncoder();
    await checkLifetime(
      stream.readable,
      (chunk) => writer.write(encoder.encode(chunk)),
      () => writer.close()
    );
  },
};

// A stream with a JavaScript source.
export const queuedStreamLifetime = {
  async test() {
    let controller;
    const readable = new ReadableStream({
      start(value) {
        controller = value;
      },
    });
    const encoder = new TextEncoder();
    await checkLifetime(
      readable,
      (chunk) => controller.enqueue(encoder.encode(chunk)),
      () => controller.close()
    );
  },
};

// In a Durable Object, inside blockConcurrencyWhile().
export class EventSourceGcDo extends DurableObject {
  async trigger() {
    await this.ctx.blockConcurrencyWhile(async () => {
      const response = await this.env.subrequest.fetch('http://example.org/gc');
      const ended = Promise.withResolvers();
      let source = EventSource.from(response.body);
      source.onerror = () => ended.resolve();
      await ended.promise;

      const sourceRef = new WeakRef(source);
      source = null;
      await collect(sourceRef);
      strictEqual(sourceRef.deref(), undefined);
    });
    await scheduler.wait(0);
  }
}

export const collectableInADurableObject = {
  async test(ctrl, env) {
    await env.ns.getByName('test').trigger();
  },
};
