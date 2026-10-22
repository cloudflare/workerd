// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, ok, strictEqual } from 'node:assert';

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

  // The live pump must retain its EventSource while a read is pending.
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

  // Settled pump callbacks must not leave a strong ownership cycle behind.
  const collected = gc({ type: 'major', execution: 'async' });
  await scheduler.wait(0);
  await collected;
  strictEqual(weakSource.deref(), undefined);
}

export const nativePumpGcLifetime = {
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

export const queuedPumpGcLifetime = {
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
