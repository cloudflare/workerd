// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Compression streams composed with Response/Request bodies, including
// through REAL HTTP serialization via the SELF loopback binding (the
// close-signal propagation from JS streams into internal response-body
// writables only manifests on an actual request/response round trip).

import { strictEqual } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

const expected = 'hello world '.repeat(100);

async function decodeAll(readable) {
  let result = '';
  for await (const chunk of readable) {
    result += dec.decode(chunk, { stream: true });
  }
  return result + dec.decode();
}

// Loopback fetch handler; main.js exports this as the default handler.
export async function handleFetch(request, env) {
  const url = request.url;
  if (url.includes('/compressed')) {
    const { readable, writable } = new CompressionStream('gzip');
    const writer = writable.getWriter();
    await writer.write(enc.encode(expected));
    await writer.close();
    return new Response(readable, {
      headers: { 'Content-Encoding': 'gzip' },
    });
  }
  if (url.includes('/stream')) {
    return new Response(expected);
  }
  if (url.includes('/compressionPipeline')) {
    // TransformStream → CompressionStream → Response body (internal
    // writable): exercises close-signal propagation from a JS-backed
    // stream through the compressor into HTTP serialization.
    const response = await env.SELF.fetch('http://test/stream');
    const { readable, writable } = new TransformStream();
    response.body.pipeTo(writable);
    const compressed = readable.pipeThrough(new CompressionStream('gzip'));
    return new Response(compressed, { encodeBody: 'manual' });
  }
  if (url.includes('/decompressionPipeline')) {
    // DecompressionStream readable → Response body (internal writable).
    const response = await env.SELF.fetch('http://test/compressed');
    const decompressed = response.body.pipeThrough(
      new DecompressionStream('gzip')
    );
    return new Response(decompressed);
  }
  return new Response('Not found', { status: 404 });
}

export const responseBodyThroughDecompression = {
  async test(_ctrl, env) {
    const response = await env.SELF.fetch('http://test/compressed');
    strictEqual(
      await decodeAll(
        response.body.pipeThrough(new DecompressionStream('gzip'))
      ),
      expected
    );
  },
};

export const decompressThroughTransformsToIdentity = {
  async test(_ctrl, env) {
    // body → DecompressionStream → TransformStream → IdentityTransformStream
    // with the pipeTo completing (not hanging).
    const response = await env.SELF.fetch('http://test/compressed');
    const ts = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(chunk);
      },
    });
    const { readable, writable } = new IdentityTransformStream();
    const pipePromise = response.body
      .pipeThrough(new DecompressionStream('gzip'))
      .pipeThrough(ts)
      .pipeTo(writable);
    strictEqual(await decodeAll(readable), expected);
    await pipePromise;
  },
};

export const compressionPipeline = {
  async test(_ctrl, env) {
    const response = await env.SELF.fetch('http://test/compressionPipeline');
    strictEqual(response.status, 200);
    strictEqual(
      await decodeAll(
        response.body.pipeThrough(new DecompressionStream('gzip'))
      ),
      expected
    );
  },
};

export const decompressionPipeline = {
  async test(_ctrl, env) {
    const response = await env.SELF.fetch('http://test/decompressionPipeline');
    strictEqual(response.status, 200);
    strictEqual(await decodeAll(response.body), expected);
  },
};
