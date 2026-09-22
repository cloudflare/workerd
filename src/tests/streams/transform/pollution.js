// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Object.prototype members must not stand in for an omitted transformer or
// strategy, nor reach the internal source and sink dictionaries a
// TransformStream is built from. The C++ implementation reads none of them;
// the TypeScript one must not either. Every mutation is undone before any
// assertion runs.

import { strictEqual, deepStrictEqual } from 'node:assert';

function pollute(entries) {
  for (const key of Object.keys(entries)) {
    Object.defineProperty(Object.prototype, key, {
      value: entries[key],
      configurable: true,
      writable: true,
    });
  }
  return () => {
    for (const key of Object.keys(entries)) {
      Reflect.deleteProperty(Object.prototype, key);
    }
  };
}

// Reads each chunk as it is written: with no readable backpressure slack, a
// write only settles once its output has been read.
async function roundTrip(transform, chunks) {
  const writer = transform.writable.getWriter();
  const reader = transform.readable.getReader();
  const out = [];
  for (const chunk of chunks) {
    const written = writer.write(chunk);
    out.push((await reader.read()).value);
    await written;
  }
  await writer.close();
  out.push((await reader.read()).done);
  return out;
}

export const omittedDictionariesReadNothing = {
  async test() {
    let pollutedCalls = 0;
    const restore = pollute({
      type: 'bytes',
      readableType: 'bytes',
      writableType: 'bytes',
      start() {
        pollutedCalls++;
      },
      transform(chunk, controller) {
        pollutedCalls++;
        controller.enqueue('POLLUTED');
      },
      flush() {
        pollutedCalls++;
      },
      size() {
        return 100;
      },
      highWaterMark: 7,
    });
    let identity;
    let custom;
    let desired;
    try {
      identity = await roundTrip(new TransformStream(), ['a', 'b']);
      const ts = new TransformStream({
        __proto__: null,
        transform(chunk, controller) {
          controller.enqueue(chunk + '!');
        },
      });
      const probe = ts.writable.getWriter();
      desired = probe.desiredSize;
      probe.releaseLock();
      custom = await roundTrip(ts, ['c']);
    } finally {
      restore();
    }
    strictEqual('type' in {}, false, 'pollution must be removed');
    strictEqual(pollutedCalls, 0);
    deepStrictEqual(identity, ['a', 'b', true]);
    deepStrictEqual(custom, ['c!', true]);
    strictEqual(desired, 1);
  },
};
