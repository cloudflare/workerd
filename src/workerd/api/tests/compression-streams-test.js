// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, rejects } from 'node:assert';

// Test CompressionStream/DecompressionStream with gzip
export const compressionGzip = {
  async test() {
    let output;
    const testData = 'hello'.repeat(100);
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    {
      const { writable, readable } = new CompressionStream('gzip');

      const writer = writable.getWriter();
      const reader = readable.getReader();

      await writer.write(enc.encode(testData));
      await writer.close();

      output = await reader.read();
    }

    {
      const { writable, readable } = new DecompressionStream('gzip');

      const writer = writable.getWriter();
      const reader = readable.getReader();

      await writer.write(output.value);
      await writer.close();

      const result = await reader.read();

      strictEqual(dec.decode(result.value), testData);
    }
  },
};

// Test CompressionStream/DecompressionStream with deflate
export const compressionDeflate = {
  async test() {
    let output;
    const testData = 'hello'.repeat(100);
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    {
      const { writable, readable } = new CompressionStream('deflate');

      const writer = writable.getWriter();
      const reader = readable.getReader();

      await writer.write(enc.encode(testData));
      await writer.close();

      output = await reader.read();
    }

    {
      const { writable, readable } = new DecompressionStream('deflate');

      const writer = writable.getWriter();
      const reader = readable.getReader();

      await writer.write(output.value);
      await writer.close();

      const result = await reader.read();

      strictEqual(dec.decode(result.value), testData);
    }
  },
};

// Test CompressionStream/DecompressionStream with deflate-raw
export const compressionDeflateRaw = {
  async test() {
    let output;
    const testData = 'hello'.repeat(100);
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    {
      const { writable, readable } = new CompressionStream('deflate-raw');

      const writer = writable.getWriter();
      const reader = readable.getReader();

      await writer.write(enc.encode(testData));
      await writer.close();

      output = await reader.read();
    }

    {
      const { writable, readable } = new DecompressionStream('deflate-raw');

      const writer = writable.getWriter();
      const reader = readable.getReader();

      await writer.write(output.value);
      await writer.close();

      const result = await reader.read();

      strictEqual(dec.decode(result.value), testData);
    }
  },
};

// Test compression/decompression with pending read
export const compressionPendingRead = {
  async test() {
    const testData = 'hello';
    const check = new Uint8Array([
      0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x06, 0x2c, 0x02,
      0x15,
    ]);
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    {
      const { writable, readable } = new CompressionStream('deflate');

      const writer = writable.getWriter();
      const reader = readable.getReader();

      const read = reader.read();

      await writer.write(enc.encode(testData));
      await writer.close();

      await read;
    }

    {
      const { writable, readable } = new DecompressionStream('deflate');

      const writer = writable.getWriter();
      const reader = readable.getReader();

      const read = reader.read();

      await writer.write(check);
      await writer.close();

      const result = await read;

      strictEqual(dec.decode(result.value), testData);
    }
  },
};

// Test cancel/abort behavior
export const compressionCancelAbort = {
  async test() {
    {
      const { writable, readable } = new CompressionStream('deflate');
      const reader = readable.getReader();
      const writer = writable.getWriter();
      const promise = reader.read();
      writer.abort(new Error('boom'));
      await rejects(promise, { message: 'boom' });
    }

    {
      const { readable } = new CompressionStream('deflate');
      const reader = readable.getReader();
      const promise = reader.read();
      reader.cancel(new Error('boom'));
      await rejects(promise, { message: 'boom' });
    }
  },
};

// Test decompression error handling
export const decompressionError = {
  async test() {
    // The data is not compressed so DecompressionStream will fail.
    const { writable, readable } = new DecompressionStream('deflate');

    const writer = writable.getWriter();
    const reader = readable.getReader();

    // Write uncompressed data which should cause a decompression error
    // The write itself may also reject, so we need to handle that too
    const writePromise = writer
      .write(new TextEncoder().encode('not compressed data'))
      .catch(() => {});

    // The read should fail with a TypeError
    await rejects(reader.read(), TypeError);

    // A second attempt to read also fails
    await rejects(reader.read(), TypeError);

    // Ensure the write operation completes before the test ends
    await writePromise;
  },
};

// Test decompression error via iteration
export const decompressionErrorIteration = {
  async test() {
    const { writable, readable } = new DecompressionStream('deflate');

    const writer = writable.getWriter();

    // Write uncompressed data which should cause a decompression error
    writer.write(new TextEncoder().encode('not compressed data'));

    const consume = async () => {
      for await (const _ of readable) {
        // Should not reach here
      }
    };

    await rejects(consume(), TypeError);
  },
};

// Test piped decompression with bad data does not hang
export const pipedDecompressionBadData = {
  async test() {
    async function consume(readable) {
      const readAll = async () => {
        for await (const _ of readable) {
          // Should error
        }
      };
      await rejects(readAll(), { message: 'Decompression failed.' });
    }

    async function doTest(transform) {
      const { writable, readable } = new DecompressionStream('gzip');

      const dest = readable.pipeThrough(transform);

      const c = consume(dest);

      const enc = new TextEncoder();
      const writer = writable.getWriter();
      await rejects(writer.write(enc.encode('hello world')), {
        message: 'Decompression failed.',
      });

      await c;
    }

    await Promise.all([
      doTest(new IdentityTransformStream()),
      doTest(new TransformStream()),
    ]);
  },
};

// Test TransformStream pipeThrough CompressionStream
export const transformStreamThroughCompression = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    // Create source data
    const sourceData = 'hello world test data';

    // Create a TransformStream as source
    const { readable, writable } = new TransformStream();

    // Pipe through compression
    const compressed = readable.pipeThrough(new CompressionStream('gzip'));

    // Pipe through decompression
    const decompressed = compressed.pipeThrough(
      new DecompressionStream('gzip')
    );

    // Write source data
    const writer = writable.getWriter();
    await writer.write(enc.encode(sourceData));
    await writer.close();

    // Read result
    let result = '';
    for await (const chunk of decompressed) {
      result += dec.decode(chunk, { stream: true });
    }
    result += dec.decode();

    strictEqual(result, sourceData);
  },
};

// Test request.body pipeThrough DecompressionStream
// This test requires a service binding that sends gzip-compressed data
export const requestBodyPipethrough = {
  async test(_ctrl, env) {
    // Fetch compressed data from the service
    const response = await env.SERVICE.fetch('http://test/compressed');

    // Pipe the response body through decompression
    const decompressed = response.body.pipeThrough(
      new DecompressionStream('gzip')
    );

    // Read all decompressed data
    const dec = new TextDecoder();
    let result = '';
    for await (const chunk of decompressed) {
      result += dec.decode(chunk, { stream: true });
    }
    result += dec.decode();

    strictEqual(result, 'hello world '.repeat(100));
  },
};

// Test transform roundtrip: request body → DecompressionStream → TransformStream → IdentityTransformStream
export const transformRoundtrip = {
  async test(_ctrl, env) {
    // Fetch compressed data from the service
    const response = await env.SERVICE.fetch('http://test/compressed');

    const decompression = new DecompressionStream('gzip');
    const ts = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(chunk);
      },
    });
    const { readable, writable } = new IdentityTransformStream();

    // Test that piping from a JS-backed TransformStream through an
    // IdentityTransformStream does not result in a hung pipeTo promise.
    const pipePromise = response.body
      .pipeThrough(decompression)
      .pipeThrough(ts)
      .pipeTo(writable);

    // Read the result
    const dec = new TextDecoder();
    let result = '';
    for await (const chunk of readable) {
      result += dec.decode(chunk, { stream: true });
    }
    result += dec.decode();

    await pipePromise;

    strictEqual(result, 'hello world '.repeat(100));
  },
};

// Test piping JS-backed stream to internal Response body.
// Regression test for close signal propagation from JS ReadableStream to internal writable.
export const compressionPipeline = {
  async test(_ctrl, env) {
    const response = await env.SERVICE.fetch('http://test/compressionPipeline');
    strictEqual(response.status, 200);

    const decompressed = response.body.pipeThrough(
      new DecompressionStream('gzip')
    );

    const dec = new TextDecoder();
    let result = '';
    for await (const chunk of decompressed) {
      result += dec.decode(chunk, { stream: true });
    }
    result += dec.decode();

    strictEqual(result, 'hello world '.repeat(100));
  },
};

// Test DecompressionStream readable piped to internal Response body.
export const decompressionPipeline = {
  async test(_ctrl, env) {
    const response = await env.SERVICE.fetch(
      'http://test/decompressionPipeline'
    );
    strictEqual(response.status, 200);

    const dec = new TextDecoder();
    let result = '';
    for await (const chunk of response.body) {
      result += dec.decode(chunk, { stream: true });
    }
    result += dec.decode();

    strictEqual(result, 'hello world '.repeat(100));
  },
};

// Test strictCompression: closing without finishing decompression should error
export const strictCompressionCloseWithoutFinish = {
  async test() {
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    await rejects(writer.close(), TypeError);
  },
};

// Test strictCompression: trailing data after valid gzip stream should error
export const strictCompressionTrailingData = {
  async test() {
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();

    // Gzipped string "FOOBAR", plus a trailing 0xFF byte
    const trailingStrm = new Uint8Array([
      0x1f, 0x8b, 0x08, 0x00, 0xf9, 0x05, 0xb7, 0x59, 0x00, 0x03, 0x4b, 0xcb,
      0xcf, 0x4f, 0x4a, 0x2c, 0x02, 0x00, 0x95, 0x1f, 0xf6, 0x9e, 0x06, 0x00,
      0x00, 0x00, 0xff,
    ]);

    await rejects(writer.write(trailingStrm), TypeError);
  },
};

export default {
  async fetch(request, env) {
    if (request.url.includes('/compressed')) {
      const data = 'hello world '.repeat(100);
      const enc = new TextEncoder();
      const { readable, writable } = new CompressionStream('gzip');
      const writer = writable.getWriter();
      await writer.write(enc.encode(data));
      await writer.close();
      return new Response(readable, {
        headers: { 'Content-Encoding': 'gzip' },
      });
    }
    if (request.url.includes('/stream')) {
      return new Response('hello world '.repeat(100));
    }
    if (request.url.includes('/compressionPipeline')) {
      // Pipe through TransformStream → CompressionStream → Response body (internal writable)
      const response = await env.SERVICE.fetch('http://test/stream');
      const { readable, writable } = new TransformStream();
      response.body.pipeTo(writable);
      const compressed = readable.pipeThrough(new CompressionStream('gzip'));
      return new Response(compressed, { encodeBody: 'manual' });
    }
    if (request.url.includes('/decompressionPipeline')) {
      // Pipe DecompressionStream readable → Response body (internal writable)
      const response = await env.SERVICE.fetch('http://test/compressed');
      const decompressed = response.body.pipeThrough(
        new DecompressionStream('gzip')
      );
      return new Response(decompressed);
    }
    return new Response('Not found', { status: 404 });
  },
};
