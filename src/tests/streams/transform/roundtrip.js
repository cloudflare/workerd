// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression: piping from a JS-backed TransformStream through an
// IdentityTransformStream must not hang the pipeTo promise. Migrated
// from transform-streams-test.js.

import { strictEqual } from 'node:assert';

export const transformRoundtrip = {
  async test(ctrl, env, ctx) {
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    const testData = 'hello world test data';
    const compressedStream = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode(testData));
        controller.close();
      },
    }).pipeThrough(new CompressionStream('gzip'));

    const compressedChunks = [];
    const compressedReader = compressedStream.getReader();
    for (;;) {
      const { done, value } = await compressedReader.read();
      if (done) break;
      compressedChunks.push(value);
    }
    const compressedData = new Uint8Array(
      compressedChunks.reduce((acc, chunk) => acc + chunk.length, 0)
    );
    let offset = 0;
    for (const chunk of compressedChunks) {
      compressedData.set(chunk, offset);
      offset += chunk.length;
    }

    const inputStream = new ReadableStream({
      start(controller) {
        controller.enqueue(compressedData);
        controller.close();
      },
    });

    const decompression = new DecompressionStream('gzip');
    const ts = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(chunk);
      },
    });
    const { readable, writable } = new IdentityTransformStream();

    ctx.waitUntil(
      inputStream.pipeThrough(decompression).pipeThrough(ts).pipeTo(writable)
    );

    const outputChunks = [];
    const reader = readable.getReader();
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      outputChunks.push(value);
    }

    const output = dec.decode(
      new Uint8Array(
        outputChunks.reduce((acc, chunk) => [...acc, ...chunk], [])
      )
    );
    strictEqual(output, testData);
  },
};
