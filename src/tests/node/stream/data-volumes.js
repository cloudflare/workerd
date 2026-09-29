// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Volumes and shapes through the adapters: many tiny chunks, alternating
// tiny and large ones, one very large chunk, and thousands of objectMode
// chunks — every byte and every object accounted for, in order.

import { Readable, Writable, pipeline } from 'node:stream';
import { strictEqual, deepStrictEqual } from 'node:assert';

const MODULUS = 251;

function patternChunk(offset, length) {
  const chunk = new Uint8Array(length);
  for (let i = 0; i < length; i++) chunk[i] = (offset + i) % MODULUS;
  return chunk;
}

function checkPattern(bytes, offset = 0) {
  for (let i = 0; i < bytes.length; i++) {
    if (bytes[i] !== (offset + i) % MODULUS) {
      strictEqual(bytes[i], (offset + i) % MODULUS, `byte ${offset + i}`);
    }
  }
}

// Ten thousand one-byte pushes through Readable.toWeb, read as a Response
// body: every byte, in order.
export const tenThousandTinyChunksThroughToWeb = {
  async test() {
    const TOTAL = 10_000;
    let pushed = 0;
    const source = new Readable({
      read() {
        for (let i = 0; i < 100 && pushed < TOTAL; i++, pushed++) {
          this.push(patternChunk(pushed, 1));
        }
        if (pushed === TOTAL) this.push(null);
      },
    });
    const bytes = new Uint8Array(
      await new Response(Readable.toWeb(source)).arrayBuffer()
    );
    strictEqual(bytes.length, TOTAL);
    checkPattern(bytes);
  },
};

// One-byte and 64 KiB chunks alternating through Writable.fromWeb: the sink
// sees the stream's bytes continuous, whatever the chunking.
export const alternatingTinyAndLargeThroughFromWeb = {
  async test() {
    let received = 0;
    const sink = new WritableStream({
      write(chunk) {
        checkPattern(chunk, received);
        received += chunk.length;
      },
    });
    const writable = Writable.fromWeb(sink);
    let offset = 0;
    for (let i = 0; i < 40; i++) {
      const length = i % 2 === 0 ? 1 : 64 * 1024;
      const flushed = new Promise((resolve) =>
        writable.write(patternChunk(offset, length), resolve)
      );
      offset += length;
      await flushed;
    }
    await new Promise((resolve) => writable.end(resolve));
    strictEqual(received, offset);
  },
};

// A single 8 MiB chunk each way: through Readable.toWeb it reaches the
// reader whole (copied), through Writable.fromWeb it reaches the sink whole
// (by reference).
export const eightMebibyteChunkBothWays = {
  async test() {
    const SIZE = 8 * 1024 * 1024;
    const big = patternChunk(0, SIZE);

    const source = new Readable({
      read() {
        this.push(big);
        this.push(null);
      },
    });
    const reader = Readable.toWeb(source).getReader();
    const parts = [];
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      parts.push(value);
    }
    strictEqual(
      parts.reduce((n, p) => n + p.length, 0),
      SIZE
    );
    let offset = 0;
    for (const part of parts) {
      checkPattern(part, offset);
      offset += part.length;
    }

    const seen = [];
    const writable = Writable.fromWeb(
      new WritableStream({
        write(chunk) {
          seen.push(chunk);
        },
      })
    );
    await new Promise((resolve) => writable.write(big, resolve));
    strictEqual(seen.length, 1);
    strictEqual(seen[0].length, SIZE);
    strictEqual(seen[0].buffer, big.buffer);
  },
};

// Ten thousand indexed objects from a web source through pipeline() into an
// objectMode Writable: none lost, none duplicated, in order.
export const objectModeIndexedThroughPipeline = {
  async test() {
    const TOTAL = 10_000;
    let next = 0;
    const source = new ReadableStream({
      pull(controller) {
        for (let i = 0; i < 100 && next < TOTAL; i++) {
          controller.enqueue({ index: next++ });
        }
        if (next === TOTAL) controller.close();
      },
    });
    const indices = [];
    const sink = new Writable({
      objectMode: true,
      write(chunk, encoding, callback) {
        indices.push(chunk.index);
        callback();
      },
    });
    await new Promise((resolve, reject) =>
      pipeline(source, sink, (err) => (err ? reject(err) : resolve()))
    );
    strictEqual(indices.length, TOTAL);
    deepStrictEqual(
      indices.filter((index, i) => index !== i),
      []
    );
  },
};
