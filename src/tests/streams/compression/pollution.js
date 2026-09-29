// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Object.prototype members must not reach the internal source, sink and
// strategy dictionaries a CompressionStream or DecompressionStream is built
// from. The C++ implementation reads none of them; the TypeScript one must
// not either. Every mutation is undone before any assertion runs.

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
    let text;
    try {
      const compressed = new Response('hello, hello, hello').body.pipeThrough(
        new CompressionStream('gzip')
      );
      const inflated = compressed.pipeThrough(new DecompressionStream('gzip'));
      text = await new Response(inflated).text();
    } finally {
      restore();
    }
    strictEqual('type' in {}, false, 'pollution must be removed');
    strictEqual(pollutedCalls, 0);
    strictEqual(text, 'hello, hello, hello');
  },
};
