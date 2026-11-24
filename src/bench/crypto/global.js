// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Batches of crypto.getRandomValues() and crypto.randomUUID() calls, which uuid libraries make
// on hot paths. Run against the C++ Crypto (global-cpp) and the TypeScript one
// (global-ts).

// Each case makes this many calls, so that the per-call cost is well above the runner's own.
const BATCH = 64;

function batch(fn) {
  return () => {
    let last;
    for (let i = 0; i < BATCH; i++) last = fn();
    return last;
  };
}

export default {
  bench(b) {
    const small = new Uint8Array(16);
    const medium = new Uint8Array(1024);
    const large = new Uint8Array(65536);
    const words = new Uint32Array(4);

    b.run(
      'getRandomValues 16 B',
      batch(() => crypto.getRandomValues(small))
    );
    b.run(
      'getRandomValues 16 B new',
      batch(() => crypto.getRandomValues(new Uint8Array(16)))
    );
    b.run(
      'getRandomValues Uint32Array(4)',
      batch(() => crypto.getRandomValues(words))
    );
    b.run(
      'getRandomValues 1 KiB',
      batch(() => crypto.getRandomValues(medium))
    );
    b.run(
      'getRandomValues 64 KiB',
      batch(() => crypto.getRandomValues(large))
    );
    b.run(
      'randomUUID',
      batch(() => crypto.randomUUID())
    );
    b.run(
      'crypto.subtle',
      batch(() => crypto.subtle)
    );
  },
};
