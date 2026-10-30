// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Compression streams in pipeThrough() chains: a round trip from a
// JavaScript stream, and the shape of a worker compressing or decompressing
// a body into a new Response.

import {
  PAYLOAD,
  chunked,
  compress,
  expectCompressedSize,
  expectEqual,
} from 'helpers';

function valueStream(chunks) {
  return new ReadableStream({
    start(controller) {
      for (const chunk of chunks) controller.enqueue(chunk);
      controller.close();
    },
  });
}

async function gzipBody() {
  const body = new Response(PAYLOAD).body.pipeThrough(
    new CompressionStream('gzip')
  );
  return (await new Response(body).arrayBuffer()).byteLength;
}

export const pipeline = {
  async bench(b) {
    const chunks = chunked(PAYLOAD, 4096);
    b.run('gzip round trip, 64 KiB in 4 KiB chunks', async () => {
      const reader = valueStream(chunks)
        .pipeThrough(new CompressionStream('gzip'))
        .pipeThrough(new DecompressionStream('gzip'))
        .getReader();
      let bytes = 0;
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        bytes += value.byteLength;
      }
      expectEqual(bytes, PAYLOAD.byteLength, 'bytes read');
    });

    // The body's chunks are the implementation's, and the output can depend
    // on them (see compress.js), so the expected size comes from a first
    // run.
    const compressedSize = await gzipBody();
    expectCompressedSize(compressedSize);
    b.run('Response body through gzip into a Response, 64 KiB', async () => {
      expectEqual(await gzipBody(), compressedSize, 'compressed bytes');
    });

    const compressed = await compress(PAYLOAD, 'gzip');
    b.run('Response body through gunzip into a Response, 64 KiB', async () => {
      const body = new Response(compressed).body.pipeThrough(
        new DecompressionStream('gzip')
      );
      const bytes = (await new Response(body).arrayBuffer()).byteLength;
      expectEqual(bytes, PAYLOAD.byteLength, 'decompressed bytes');
    });
  },
};
