// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Body consumption copies bytes out of the chunks as they arrive: a chunk
// is collectible before consumption ends (so a body of many tiny chunks
// costs its bytes, not an object per chunk), and the assembled result is
// exact whatever the chunk sizes, including one chunk wider than the
// blocks the bytes are copied into. Requires --expose-gc and WeakRef (both
// cell configs). PARITY throughout.

import { strictEqual, ok } from 'node:assert';

// One-byte chunks handed out per pull, their buffers tracked only weakly;
// after `before` pulls the source collects garbage and counts the
// survivors, then delivers `after` more.
export const chunksCollectibleDuringConsumption = {
  async test() {
    const before = 2000;
    const after = 2000;
    const refs = [];
    let alive;
    const rs = new ReadableStream({
      async pull(c) {
        if (refs.length === before) {
          for (let i = 0; i < 3; i++) {
            await scheduler.wait(5);
            gc();
          }
          alive = 0;
          for (const ref of refs) if (ref.deref() !== undefined) alive++;
        }
        if (refs.length < before + after) {
          const chunk = new Uint8Array([97]);
          refs.push(new WeakRef(chunk.buffer));
          c.enqueue(chunk);
        } else {
          c.close();
        }
      },
    });
    const text = await new Response(rs).text();
    strictEqual(text, 'a'.repeat(before + after));
    // The batch in flight may still be referenced; the rest must be gone.
    ok(alive <= 4, `${alive} of ${before} chunks still alive`);
  },
};

// Chunk sizes from a fixed pseudo-random sequence (1..3000 bytes, ~2.5 MiB
// in all), every byte position-encoded, read back as bytes and as text.
function oddSizedBody(total, encode) {
  let seed = 12345;
  let offset = 0;
  return new ReadableStream({
    pull(c) {
      if (offset >= total) {
        c.close();
        return;
      }
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      const size = Math.min(1 + (seed % 3000), total - offset);
      const chunk = new Uint8Array(size);
      for (let i = 0; i < size; i++) chunk[i] = encode(offset + i);
      offset += size;
      c.enqueue(chunk);
    },
  });
}

export const oddChunkSizesAssembleIntact = {
  async test() {
    const total = 2_500_000;
    const bytes = new Uint8Array(
      await new Response(oddSizedBody(total, (i) => i & 0xff)).arrayBuffer()
    );
    strictEqual(bytes.byteLength, total);
    for (let i = 0; i < total; i++) {
      if (bytes[i] !== (i & 0xff)) throw new Error(`byte ${i} is ${bytes[i]}`);
    }
    const text = await new Response(
      oddSizedBody(total, (i) => 97 + (i % 26))
    ).text();
    strictEqual(text.length, total);
    for (let i = 0; i < total; i += 997) {
      strictEqual(text.charCodeAt(i), 97 + (i % 26));
    }
    strictEqual(text.charCodeAt(total - 1), 97 + ((total - 1) % 26));
  },
};

// One chunk wider than the TypeScript implementation's largest collection
// block (1 MiB), arriving after a one-byte chunk so the block in progress
// is small: the chunk is copied out across several blocks, and the small
// chunk after it lands in the last, partial one.
export const chunkWiderThanABlockAssemblesIntact = {
  async test() {
    const wide = 2 * 1024 * 1024 + 5;
    const sizes = [1, wide, 7];
    const total = 1 + wide + 7;
    let offset = 0;
    const rs = new ReadableStream({
      pull(c) {
        if (sizes.length === 0) {
          c.close();
          return;
        }
        const size = sizes.shift();
        const chunk = new Uint8Array(size);
        for (let i = 0; i < size; i++) chunk[i] = (offset + i) & 0xff;
        offset += size;
        c.enqueue(chunk);
      },
    });
    const bytes = new Uint8Array(await new Response(rs).arrayBuffer());
    strictEqual(bytes.byteLength, total);
    for (let i = 0; i < total; i++) {
      if (bytes[i] !== (i & 0xff)) throw new Error(`byte ${i} is ${bytes[i]}`);
    }
  },
};

// A byte source that declares its total and meets it, in several chunks.
export const declaredLengthBodyIsExact = {
  async test() {
    const parts = ['hel', 'lo, ', 'wor', 'ld'];
    const rs = new ReadableStream({
      type: 'bytes',
      expectedLength: 12,
      pull(c) {
        if (parts.length === 0) c.close();
        else c.enqueue(new TextEncoder().encode(parts.shift()));
      },
    });
    const buffer = await new Response(rs).arrayBuffer();
    strictEqual(buffer.byteLength, 12);
    strictEqual(new TextDecoder().decode(buffer), 'hello, world');
  },
};
