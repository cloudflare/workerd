// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for the TypeScript CompressionStream/DecompressionStream
// implementation (typescript_implemented_streams + the per-isolate
// bootstrap), backed by the synchronous C++ codec handle. Covers the
// deliberate semantics documented in webstreams/compression.ts:
// eager (transform-time) error timing, legacy-parity write settlement,
// and the byte-capable (BYOB) readable.
//
// The config enables strict_compression_checks, matching production
// defaults, so the strict flush-time checks are exercised too.

import { strictEqual, deepStrictEqual, ok, rejects, throws } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

function userSource(...chunks) {
  return new ReadableStream({
    start(controller) {
      for (const chunk of chunks) {
        controller.enqueue(chunk);
      }
      controller.close();
    },
  });
}

async function readAll(readable) {
  const reader = readable.getReader();
  const chunks = [];
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    chunks.push(value);
  }
  return chunks;
}

function toBytes(chunks) {
  return chunks.flatMap((chunk) => [...chunk]);
}

// Writes the chunks, closes, and drains the readable; returns the byte
// array. Writes settle without reads (legacy-parity settlement), so the
// sequential form is safe.
async function pump(pair, chunks) {
  const writer = pair.writable.getWriter();
  for (const chunk of chunks) {
    await writer.write(chunk);
  }
  await writer.close();
  return toBytes(await readAll(pair.readable));
}

// --- Core behavior ---

export const writesSettleWithoutReads = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    // LEGACY-PARITY SETTLEMENT: writes settle as soon as the codec has
    // consumed the chunk — no read required (unlike the rendezvous of
    // IdentityTransformStream).
    await writer.write(enc.encode('hello'));
    await writer.close();
    const bytes = toBytes(await readAll(cs.readable));
    // gzip magic.
    strictEqual(bytes[0], 0x1f);
    strictEqual(bytes[1], 0x8b);
  },
};

export const roundTripAllFormats = {
  async test() {
    const payload = 'The quick brown fox jumps over the lazy dog. '.repeat(50);
    for (const format of ['gzip', 'deflate', 'deflate-raw']) {
      const data = enc.encode(payload);
      const compressed = await pump(new CompressionStream(format), [data]);
      // The codec actually compressed the repetitive payload.
      strictEqual(compressed.length < data.length, true);
      const restored = await pump(new DecompressionStream(format), [
        new Uint8Array(compressed),
      ]);
      strictEqual(dec.decode(new Uint8Array(restored)), payload);
    }
  },
};

export const pipeThroughInterop = {
  async test() {
    const payload = enc.encode('compress me via pipes');
    const out = toBytes(
      await readAll(
        userSource(payload)
          .pipeThrough(new CompressionStream('deflate'))
          .pipeThrough(new DecompressionStream('deflate'))
      )
    );
    deepStrictEqual(out, [...payload]);
  },
};

export const chunkedWritesRoundTrip = {
  async test() {
    // Multi-chunk writes exercise the stateful streaming path.
    const parts = ['first ', 'second ', 'third'];
    const compressed = await pump(
      new CompressionStream('gzip'),
      parts.map((p) => enc.encode(p))
    );
    // Split the compressed bytes into awkward chunks for decompression.
    const half = compressed.length >> 1;
    const restored = await pump(new DecompressionStream('gzip'), [
      new Uint8Array(compressed.slice(0, half)),
      new Uint8Array(compressed.slice(half)),
    ]);
    strictEqual(dec.decode(new Uint8Array(restored)), parts.join(''));
  },
};

// --- Error timing ---

export const corruptInputRejectsWrite = {
  async test() {
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    // Transform-time error timing: the WRITE itself rejects, without
    // any read on the readable side ever happening.
    await rejects(writer.write(enc.encode('definitely not gzip')), TypeError);
    // The readable side is errored too (both-sides propagation).
    await rejects(readAll(ds.readable), TypeError);
  },
};

export const strictIncompleteCloseRejects = {
  async test() {
    // Build a valid gzip payload, then truncate the trailer so every
    // WRITE succeeds but the stream is incomplete at close.
    const compressed = await pump(new CompressionStream('gzip'), [
      enc.encode('hello world'),
    ]);
    const truncated = new Uint8Array(
      compressed.slice(0, compressed.length - 4)
    );
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    await writer.write(truncated);
    // Flush-time error timing: the strict incomplete-data check rejects
    // the CLOSE (strict_compression_checks is enabled in this config).
    await rejects(writer.close(), TypeError);
  },
};

export const invalidFormatThrows = {
  test() {
    throws(() => new CompressionStream('br'), /must be either/);
    // Missing argument coerces to "undefined" — same TypeError.
    throws(() => new DecompressionStream(), /must be either/);
  },
};

// --- Legacy-parity surface ---

export const byobReadSupported = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    await writer.write(enc.encode('byob'));
    await writer.close();
    // Legacy parity: the readable is byte-capable, so BYOB readers work
    // (WHATWG describes a default stream here; the legacy C++ pair was
    // BYOB-readable and this implementation preserves that).
    const reader = cs.readable.getReader({ mode: 'byob' });
    const { value, done } = await reader.read(new Uint8Array(2), { min: 2 });
    strictEqual(done, false);
    deepStrictEqual([...value], [0x1f, 0x8b]);
    await reader.cancel();
  },
};

export const classShape = {
  test() {
    strictEqual(
      Object.prototype.toString.call(new CompressionStream('gzip')),
      '[object CompressionStream]'
    );
    strictEqual(
      Object.prototype.toString.call(new DecompressionStream('gzip')),
      '[object DecompressionStream]'
    );
    // The internal C++ codec factory is not on the class: it is injected
    // through the bootstrap's utils pseudo-global, never a JS-visible
    // surface.
    strictEqual('newCodec' in CompressionStream, false);
    strictEqual('newCodec' in DecompressionStream, false);
    // Brand checks: prototype getters reject foreign receivers.
    const desc = Object.getOwnPropertyDescriptor(
      CompressionStream.prototype,
      'readable'
    );
    throws(() => desc.get.call({}), TypeError);
  },
};

export const abortErrorsBothSides = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    await writer.write(enc.encode('partial'));
    const reason = new Error('abandon');
    await writer.abort(reason);
    await rejects(readAll(cs.readable), /abandon/);
  },
};

export const cancelReadableErrorsWritable = {
  async test() {
    const cs = new CompressionStream('gzip');
    const reason = new Error('no more');
    await cs.readable.cancel(reason);
    // Mirror of the identity pair: cancelling the readable errors the
    // writable side.
    const writer = cs.writable.getWriter();
    await rejects(writer.closed, /no more/);
  },
};

// Concatenates an array of Uint8Array chunks (byte-count-preserving, unlike
// toBytes' number-array form, which is convenient only for small payloads).
function concat(chunks) {
  const total = chunks.reduce((sum, c) => sum + c.byteLength, 0);
  const out = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    out.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return out;
}

// Large multi-pump payload (many 16KiB codec pump iterations, chunked writes):
// pure in-worker coverage, no fetch involved.
export const largePayloadRoundtrip = {
  async test() {
    const size = 400 * 1024;
    const original = new Uint8Array(size);
    // Compressible but non-trivial content.
    for (let i = 0; i < size; i++)
      original[i] = (i * 31 + ((i / 512) | 0)) & 0xff;

    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    const writes = (async () => {
      // Chunked writes to exercise repeated eager pushes.
      for (let off = 0; off < size; off += 64 * 1024) {
        await writer.write(
          original.subarray(off, Math.min(off + 64 * 1024, size))
        );
      }
      await writer.close();
    })();
    const compressed = concat(await readAll(cs.readable));
    await writes;
    ok(
      compressed.byteLength < size,
      `compressed output should be smaller: ${compressed.byteLength} vs ${size}`
    );

    const ds = new DecompressionStream('gzip');
    const writer2 = ds.writable.getWriter();
    const writes2 = (async () => {
      await writer2.write(compressed);
      await writer2.close();
    })();
    const roundtrip = concat(await readAll(ds.readable));
    await writes2;
    strictEqual(roundtrip.byteLength, size);
    deepStrictEqual(roundtrip, original);
  },
};
