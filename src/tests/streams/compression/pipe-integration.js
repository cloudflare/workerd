// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Compression streams composed with other streams via pipeThrough/pipeTo.

import { strictEqual, rejects } from 'node:assert';

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

async function decodeAll(readable) {
  let result = '';
  for await (const chunk of readable) {
    result += dec.decode(chunk, { stream: true });
  }
  return result + dec.decode();
}

export const compressDecompressPipeChain = {
  async test() {
    const payload = 'compress me via pipes';
    const out = await decodeAll(
      userSource(enc.encode(payload))
        .pipeThrough(new CompressionStream('deflate'))
        .pipeThrough(new DecompressionStream('deflate'))
    );
    strictEqual(out, payload);
  },
};

export const transformSourceThroughCompression = {
  async test() {
    // A plain TransformStream as the source of a compress→decompress chain.
    const sourceData = 'hello world test data';
    const { readable, writable } = new TransformStream();
    const decompressed = readable
      .pipeThrough(new CompressionStream('gzip'))
      .pipeThrough(new DecompressionStream('gzip'));
    const writer = writable.getWriter();
    await writer.write(enc.encode(sourceData));
    await writer.close();
    strictEqual(await decodeAll(decompressed), sourceData);
  },
};

export const compressionThroughIdentity = {
  async test() {
    const original = 'Piping compression to identity transform.';
    const compressed = userSource(enc.encode(original))
      .pipeThrough(new CompressionStream('deflate'))
      .pipeThrough(new IdentityTransformStream());
    const decompressed = compressed.pipeThrough(
      new DecompressionStream('deflate')
    );
    strictEqual(await decodeAll(decompressed), original);
  },
};

export const badDataPropagatesThroughPipes = {
  async test() {
    // A decompression failure propagates through a downstream transform to
    // the consumer instead of hanging it, for both an identity transform
    // and a standard TransformStream.
    async function doTest(transform) {
      const { writable, readable } = new DecompressionStream('gzip');
      const dest = readable.pipeThrough(transform);
      const consumed = rejects(
        (async () => {
          for await (const chunk of dest) {
            void chunk;
          }
        })(),
        { message: 'Decompression failed.' }
      );
      const writer = writable.getWriter();
      await rejects(writer.write(enc.encode('hello world')), {
        message: 'Decompression failed.',
      });
      await consumed;
    }
    await Promise.all([
      doTest(new IdentityTransformStream()),
      doTest(new TransformStream()),
    ]);
  },
};
