// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The "brotli" format. Brotli gets the trailing-data and incomplete-stream
// checks whether or not strict_compression_checks is enabled: the legacy
// cell runs this module too.

import { strictEqual, rejects } from 'node:assert';
import { pump } from 'round-trip';

const enc = new TextEncoder();
const dec = new TextDecoder();

// Brotli of "expected output" (the WPT compression vector).
const expectedOutput = new Uint8Array([
  0x21, 0x38, 0x00, 0x04, 0x65, 0x78, 0x70, 0x65, 0x63, 0x74, 0x65, 0x64, 0x20,
  0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x03,
]);

export const brotliDecodesKnownVector = {
  async test() {
    const output = await pump(new DecompressionStream('brotli'), [
      expectedOutput,
    ]);
    strictEqual(dec.decode(output), 'expected output');
  },
};

export const brotliLargeRoundTrip = {
  async test() {
    // Larger than the codec's 16 KiB scratch buffer in both directions.
    const payload = enc.encode(
      Array.from({ length: 20000 }, (_, i) => `line ${i}\n`).join('')
    );
    const compressed = await pump(new CompressionStream('brotli'), [payload]);
    strictEqual(compressed.byteLength < payload.byteLength, true);
    // One byte at a time for the first few, then the rest.
    const pieces = [];
    for (let i = 0; i < 8; i++) {
      pieces.push(compressed.subarray(i, i + 1));
    }
    pieces.push(compressed.subarray(8));
    const output = await pump(new DecompressionStream('brotli'), pieces);
    strictEqual(dec.decode(output), dec.decode(payload));
  },
};

export const brotliCorruptInputRejectsWrite = {
  async test() {
    const ds = new DecompressionStream('brotli');
    const writer = ds.writable.getWriter();
    const reader = ds.readable.getReader();
    await rejects(
      writer.write(Uint8Array.of(0xff, 0xff, 0xff, 0xff, 0xff, 0xff)),
      (err) => {
        strictEqual(err.constructor, TypeError);
        strictEqual(err.message, 'Decompression failed.');
        return true;
      }
    );
    await rejects(reader.read(), TypeError);
  },
};

export const brotliTrailingDataRejectsWrite = {
  async test() {
    const ds = new DecompressionStream('brotli');
    const writer = ds.writable.getWriter();
    await rejects(
      writer.write(Uint8Array.of(...expectedOutput, 0x00)),
      (err) => {
        strictEqual(err.constructor, TypeError);
        strictEqual(err.message, 'Trailing bytes after end of compressed data');
        return true;
      }
    );
  },
};

export const brotliCloseWithoutDataRejects = {
  async test() {
    const ds = new DecompressionStream('brotli');
    const writer = ds.writable.getWriter();
    await rejects(writer.close(), TypeError);
  },
};

export const brotliTruncatedStreamRejectsClose = {
  async test() {
    const ds = new DecompressionStream('brotli');
    const writer = ds.writable.getWriter();
    await writer.write(expectedOutput.subarray(0, expectedOutput.length - 1));
    await rejects(writer.close(), (err) => {
      strictEqual(err.constructor, TypeError);
      strictEqual(
        err.message,
        'Called close() on a decompression stream with incomplete data'
      );
      return true;
    });
  },
};
