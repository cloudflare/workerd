// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// End-to-end pipe volumes: multi-MiB bodies through pipeTo and
// pipeThrough chains across JS-backed and native (identity) endpoints,
// verified byte-exact against the continuous pattern (prime modulus).

import { strictEqual } from 'node:assert';

const PATTERN_MODULUS = 251;

function patternChunk(offset, length) {
  const chunk = new Uint8Array(length);
  for (let i = 0; i < length; i++) {
    chunk[i] = (offset + i) % PATTERN_MODULUS;
  }
  return chunk;
}

function patternedSource(total, chunkLength) {
  let offset = 0;
  return new ReadableStream({
    pull(c) {
      if (offset >= total) {
        c.close();
        return;
      }
      const length = Math.min(chunkLength, total - offset);
      c.enqueue(patternChunk(offset, length));
      offset += length;
    },
  });
}

function verifyingSink() {
  const state = { received: 0, closed: false };
  const ws = new WritableStream({
    write(chunk) {
      for (let i = 0; i < chunk.byteLength; i++) {
        if (chunk[i] !== (state.received + i) % PATTERN_MODULUS) {
          strictEqual(
            chunk[i],
            (state.received + i) % PATTERN_MODULUS,
            `pattern break at byte ${state.received + i}`
          );
        }
      }
      state.received += chunk.byteLength;
    },
    close() {
      state.closed = true;
    },
  });
  return { ws, state };
}

function assertPatternedBytes(bytes, total) {
  strictEqual(bytes.byteLength, total, 'wrong total byte length');
  const view = new Uint8Array(bytes);
  for (let i = 0; i < total; i++) {
    if (view[i] !== i % PATTERN_MODULUS) {
      strictEqual(view[i], i % PATTERN_MODULUS, `pattern break at byte ${i}`);
    }
  }
}

// LARGE pipeTo, JS → JS: 1 MiB in 16 KiB chunks.
export const largePipeJsToJs = {
  async test() {
    const total = 1024 * 1024;
    const { ws, state } = verifyingSink();
    await patternedSource(total, 16 * 1024).pipeTo(ws);
    strictEqual(state.received, total);
    strictEqual(state.closed, true);
  },
};

// VERY LARGE pipeThrough chain, JS → JS transform → JS: 8 MiB in
// 64 KiB chunks.
export const veryLargePipeChain = {
  async test() {
    const total = 8 * 1024 * 1024;
    const { ws, state } = verifyingSink();
    await patternedSource(total, 64 * 1024)
      .pipeThrough(new TransformStream())
      .pipeTo(ws);
    strictEqual(state.received, total);
    strictEqual(state.closed, true);
  },
};

// LARGE pipeTo into a NATIVE identity stream, read back through a body:
// 1 MiB, byte-exact.
export const largePipeJsToIdentity = {
  async test() {
    const total = 1024 * 1024;
    const its = new IdentityTransformStream();
    const pipeP = patternedSource(total, 16 * 1024).pipeTo(its.writable);
    const bytes = await new Response(its.readable).arrayBuffer();
    assertPatternedBytes(bytes, total);
    await pipeP;
  },
};

// LARGE pipe FROM a native identity readable into a JS sink: 1 MiB fed
// by a concurrent writer.
export const largePipeIdentityToJs = {
  async test() {
    const total = 1024 * 1024;
    const its = new IdentityTransformStream();
    const producer = (async () => {
      const writer = its.writable.getWriter();
      for (let offset = 0; offset < total; offset += 16 * 1024) {
        await writer.write(patternChunk(offset, 16 * 1024));
      }
      await writer.close();
    })();
    const { ws, state } = verifyingSink();
    await Promise.all([its.readable.pipeTo(ws), producer]);
    strictEqual(state.received, total);
    strictEqual(state.closed, true);
  },
};
