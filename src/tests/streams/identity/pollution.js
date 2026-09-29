// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Object.prototype members must not reach the internal source, sink and
// strategy dictionaries an IdentityTransformStream or FixedLengthStream is
// built from, nor stand in for an omitted strategy. The C++ implementation
// reads none of them; the TypeScript one must not either. Every mutation is
// undone before any assertion runs.

import { strictEqual } from 'node:assert';

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

async function roundTrip(transform, text) {
  const writer = transform.writable.getWriter();
  const done = writer.write(new TextEncoder().encode(text)).then(() => {
    return writer.close();
  });
  const read = await new Response(transform.readable).text();
  await done;
  return read;
}

export const internalDictionariesIgnorePollution = {
  async test() {
    let pollutedCalls = 0;
    const restore = pollute({
      type: 'bytes',
      autoAllocateChunkSize: 16,
      expectedLength: 3,
      start() {
        pollutedCalls++;
      },
      size() {
        return 100;
      },
      highWaterMark: 7,
    });
    let identity;
    let fixed;
    try {
      identity = await roundTrip(new IdentityTransformStream(), 'hello');
      fixed = await roundTrip(new FixedLengthStream(5), 'hello');
    } finally {
      restore();
    }
    strictEqual('type' in {}, false, 'pollution must be removed');
    strictEqual(pollutedCalls, 0);
    strictEqual(identity, 'hello');
    strictEqual(fixed, 'hello');
  },
};
