// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression: piping from a JS-backed TransformStream through an
// IdentityTransformStream must not hang the pipeTo promise. Migrated
// from transform-streams-test.js.

import { strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

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

// The workerd TransformStream({ expectedLength }) extension: posting
// the readable as a fetch body. DIVERGENCE: C++ surfaces the declared
// length as a concrete Content-Length on the subrequest; the
// TypeScript implementation does not consult expectedLength — the
// subrequest goes out chunked (Content-Length null). The body arrives
// intact either way.
export const transformExpectedLengthFetchBody = {
  async test(ctrl, env) {
    const enc = new TextEncoder();
    const { readable, writable } = new TransformStream({
      expectedLength: 10,
    });
    const writer = writable.getWriter();
    writer.write(enc.encode('hellohello'));
    writer.close();
    const resp = await env.SELF.fetch('http://example.org', {
      method: 'POST',
      body: readable,
    });
    strictEqual(
      resp.headers.get('observed-content-length'),
      usingTsImpl ? 'null' : '10'
    );
    strictEqual(await resp.text(), 'hellohello');
  },
};

// The same through a pre-built Request object (regression for
// https://github.com/cloudflare/workerd/issues/5113: the length was
// lost on the C++ Request path).
export const transformExpectedLengthRequestBody = {
  async test(ctrl, env) {
    const enc = new TextEncoder();
    const { readable, writable } = new TransformStream({
      expectedLength: 10,
    });
    const writer = writable.getWriter();
    writer.write(enc.encode('hellohello'));
    writer.close();
    const resp = await env.SELF.fetch(
      new Request('http://example.org', {
        method: 'POST',
        body: readable,
      })
    );
    strictEqual(
      resp.headers.get('observed-content-length'),
      usingTsImpl ? 'null' : '10'
    );
    strictEqual(await resp.text(), 'hellohello');
  },
};
