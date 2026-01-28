// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for compression streams edge cases with chunked data.
// These tests focus on scenarios where data arrives in multiple chunks,
// large data handling, and error scenarios.
//
// Test inspirations:
// - Bun: test/js/web/streams/compression.test.ts (round-trip tests, various formats)
// - Deno: tests/unit/streams_test.ts (compression abort/cancel tests)
// - Bun: test/js/web/fetch/fetch.stream.test.ts (corrupted data handling)

import { strictEqual, ok, rejects } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

// Test compression with many small chunks (1 byte each)
// Inspired by: Bun test/js/web/streams/compression.test.ts
export const compressionMultipleSmallChunks = {
  async test() {
    const originalText = 'Hello, World! This is a test of chunked compression.';
    const bytes = enc.encode(originalText);

    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();

    for (let i = 0; i < bytes.length; i++) {
      await writer.write(new Uint8Array([bytes[i]]));
    }
    await writer.close();

    const compressedChunks = [];
    const reader = cs.readable.getReader();
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      compressedChunks.push(value);
    }

    const ds = new DecompressionStream('gzip');
    const dsWriter = ds.writable.getWriter();
    for (const chunk of compressedChunks) {
      await dsWriter.write(chunk);
    }
    await dsWriter.close();

    let result = '';
    const dsReader = ds.readable.getReader();
    while (true) {
      const { value, done } = await dsReader.read();
      if (done) break;
      result += dec.decode(value, { stream: true });
    }
    result += dec.decode();

    strictEqual(result, originalText);
  },
};

// Test decompression with chunked input (compressed data arrives in pieces)
// Inspired by: Bun test/js/web/fetch/fetch.stream.test.ts
export const decompressionChunkedInput = {
  async test() {
    const originalText = 'Test data for chunked decompression testing.';

    const cs = new CompressionStream('deflate');
    const csWriter = cs.writable.getWriter();
    await csWriter.write(enc.encode(originalText));
    await csWriter.close();

    const compressedChunks = [];
    const csReader = cs.readable.getReader();
    while (true) {
      const { value, done } = await csReader.read();
      if (done) break;
      compressedChunks.push(value);
    }

    const totalLength = compressedChunks.reduce(
      (sum, chunk) => sum + chunk.length,
      0
    );
    const compressed = new Uint8Array(totalLength);
    let offset = 0;
    for (const chunk of compressedChunks) {
      compressed.set(chunk, offset);
      offset += chunk.length;
    }

    const ds = new DecompressionStream('deflate');
    const dsWriter = ds.writable.getWriter();

    for (let i = 0; i < compressed.length; i += 2) {
      const end = Math.min(i + 2, compressed.length);
      await dsWriter.write(compressed.slice(i, end));
    }
    await dsWriter.close();

    let result = '';
    const dsReader = ds.readable.getReader();
    while (true) {
      const { value, done } = await dsReader.read();
      if (done) break;
      result += dec.decode(value, { stream: true });
    }
    result += dec.decode();

    strictEqual(result, originalText);
  },
};

// Test compression with large data (100KB+)
// Inspired by: Bun bench/snippets/compression-streams.mjs
export const compressionLargeData = {
  async test() {
    const size = 100 * 1024;
    const data = new Uint8Array(size);
    for (let i = 0; i < size; i++) {
      data[i] = i % 256;
    }

    const cs = new CompressionStream('gzip');
    const csWriter = cs.writable.getWriter();
    await csWriter.write(data);
    await csWriter.close();

    const compressedChunks = [];
    const csReader = cs.readable.getReader();
    while (true) {
      const { value, done } = await csReader.read();
      if (done) break;
      compressedChunks.push(value);
    }

    const compressedLength = compressedChunks.reduce(
      (sum, chunk) => sum + chunk.length,
      0
    );
    ok(compressedLength < size, 'Compressed should be smaller than original');

    const ds = new DecompressionStream('gzip');
    const dsWriter = ds.writable.getWriter();
    for (const chunk of compressedChunks) {
      await dsWriter.write(chunk);
    }
    await dsWriter.close();

    const decompressedChunks = [];
    const dsReader = ds.readable.getReader();
    while (true) {
      const { value, done } = await dsReader.read();
      if (done) break;
      decompressedChunks.push(value);
    }

    const decompressedLength = decompressedChunks.reduce(
      (sum, chunk) => sum + chunk.length,
      0
    );
    strictEqual(decompressedLength, size);

    const decompressed = new Uint8Array(decompressedLength);
    let dOffset = 0;
    for (const chunk of decompressedChunks) {
      decompressed.set(chunk, dOffset);
      dOffset += chunk.length;
    }

    for (let i = 0; i < size; i++) {
      strictEqual(decompressed[i], i % 256, `Byte at ${i} should match`);
    }
  },
};

// Test decompression with completely invalid data should error
// Inspired by: Bun test/js/web/fetch/fetch.stream.test.ts (corrupted data handling)
export const decompressionTruncated = {
  async test() {
    const invalidData = new Uint8Array([0x00, 0x01, 0x02, 0x03, 0x04, 0x05]);

    const ds = new DecompressionStream('gzip');
    const dsWriter = ds.writable.getWriter();
    const dsReader = ds.readable.getReader();

    await rejects(async () => {
      await dsWriter.write(invalidData);
      await dsWriter.close();

      while (true) {
        const { done } = await dsReader.read();
        if (done) break;
      }
    });
  },
};

// Test empty stream through compression
// Inspired by: Bun test/js/web/streams/compression.test.ts
export const compressionEmptyStream = {
  async test() {
    const cs = new CompressionStream('deflate');
    const writer = cs.writable.getWriter();
    await writer.close();

    const chunks = [];
    const reader = cs.readable.getReader();
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      chunks.push(value);
    }

    const ds = new DecompressionStream('deflate');
    const dsWriter = ds.writable.getWriter();
    for (const chunk of chunks) {
      await dsWriter.write(chunk);
    }
    await dsWriter.close();

    const dsReader = ds.readable.getReader();
    const result = await dsReader.read();
    ok(result.done);
  },
};

// Test empty decompression
// Inspired by: Bun test/js/web/streams/compression.test.ts
export const decompressionEmptyStream = {
  async test() {
    // Create valid empty compressed data first
    const cs = new CompressionStream('gzip');
    const csWriter = cs.writable.getWriter();
    await csWriter.close();

    const compressedChunks = [];
    const csReader = cs.readable.getReader();
    while (true) {
      const { value, done } = await csReader.read();
      if (done) break;
      compressedChunks.push(value);
    }

    // Now decompress
    const ds = new DecompressionStream('gzip');
    const dsWriter = ds.writable.getWriter();
    for (const chunk of compressedChunks) {
      await dsWriter.write(chunk);
    }
    await dsWriter.close();

    // Verify empty result
    const dsReader = ds.readable.getReader();
    const result = await dsReader.read();
    ok(result.done);
  },
};

// Test CompressionStream piped to IdentityTransformStream
// Inspired by: workerd compression-streams-test.js
export const compressionPipeToIdentity = {
  async test() {
    const originalText = 'Piping compression to identity transform.';

    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode(originalText));
        controller.close();
      },
    });

    const compressed = source
      .pipeThrough(new CompressionStream('deflate'))
      .pipeThrough(new IdentityTransformStream());

    const decompressed = compressed.pipeThrough(
      new DecompressionStream('deflate')
    );

    let result = '';
    for await (const chunk of decompressed) {
      result += dec.decode(chunk, { stream: true });
    }
    result += dec.decode();

    strictEqual(result, originalText);
  },
};

// Test all supported formats work with chunked data
// Inspired by: Bun test/js/web/streams/compression.test.ts
export const decompressionAllFormats = {
  async test() {
    const formats = ['gzip', 'deflate', 'deflate-raw'];
    const originalText = 'Testing all compression formats with chunked data!';

    for (const format of formats) {
      // Compress
      const cs = new CompressionStream(format);
      const csWriter = cs.writable.getWriter();

      // Write in chunks
      const bytes = enc.encode(originalText);
      for (let i = 0; i < bytes.length; i += 5) {
        await csWriter.write(bytes.slice(i, Math.min(i + 5, bytes.length)));
      }
      await csWriter.close();

      // Collect compressed
      const compressedChunks = [];
      const csReader = cs.readable.getReader();
      while (true) {
        const { value, done } = await csReader.read();
        if (done) break;
        compressedChunks.push(value);
      }

      // Decompress
      const ds = new DecompressionStream(format);
      const dsWriter = ds.writable.getWriter();
      for (const chunk of compressedChunks) {
        await dsWriter.write(chunk);
      }
      await dsWriter.close();

      // Verify
      let result = '';
      const dsReader = ds.readable.getReader();
      while (true) {
        const { value, done } = await dsReader.read();
        if (done) break;
        result += dec.decode(value, { stream: true });
      }
      result += dec.decode();

      strictEqual(
        result,
        originalText,
        `Format ${format} should round-trip correctly`
      );
    }
  },
};
