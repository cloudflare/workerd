// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The response as a node Readable in the web-streams world: piped into a
// Writable.fromWeb sink, adapted with Readable.toWeb, run through a
// TransformStream pipeline, drained by stream/consumers and async
// iteration.

import { Readable, Writable, pipeline } from 'node:stream';
import { text } from 'node:stream/consumers';
import { Buffer } from 'node:buffer';
import { strictEqual } from 'node:assert';
import { get, response } from 'harness';

const enc = new TextEncoder();
const dec = new TextDecoder();

// res.pipe(Writable.fromWeb(ws)): every chunk reaches the web sink and the
// sink closes after 'end'.
export const pipeIntoWebSink = {
  async test(ctrl, env) {
    const res = await response(get(env, '/chunked?n=3&delay=5'));
    const received = [];
    let closed = false;
    const sink = new WritableStream({
      write(chunk) {
        received.push(dec.decode(chunk));
      },
      close() {
        closed = true;
      },
    });
    const dest = Writable.fromWeb(sink);
    res.pipe(dest);
    await new Promise((resolve) => dest.on('finish', resolve));
    await scheduler.wait(5);
    strictEqual(received.join(''), 'chunk-0|chunk-1|chunk-2|');
    strictEqual(closed, true);
  },
};

// Readable.toWeb(res) as a Response body: text() yields the whole body.
export const toWebAsResponseBody = {
  async test(ctrl, env) {
    const bytes = 256 * 1024;
    const res = await response(get(env, `/large?bytes=${bytes}`));
    const body = new Uint8Array(
      await new Response(Readable.toWeb(res)).arrayBuffer()
    );
    strictEqual(body.byteLength, bytes);
    strictEqual(body[1000], 1000 % 251);
    strictEqual(res.complete, true);
  },
};

// pipeline(res, TransformStream, sink): the body through a web transform.
export const pipelineThroughWebTransform = {
  async test(ctrl, env) {
    const res = await response(get(env, '/pong'));
    const upper = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(enc.encode(dec.decode(chunk).toUpperCase()));
      },
    });
    const chunks = [];
    await new Promise((resolve, reject) => {
      pipeline(
        res,
        upper,
        new Writable({
          write(chunk, encoding, callback) {
            chunks.push(chunk);
            callback();
          },
        }),
        (err) => (err ? reject(err) : resolve())
      );
    });
    strictEqual(Buffer.concat(chunks).toString(), 'PONG');
  },
};

// stream/consumers and async iteration drain the response.
export const consumersAndAsyncIteration = {
  async test(ctrl, env) {
    strictEqual(await text(await response(get(env, '/pong'))), 'pong');
    const res = await response(get(env, '/chunked?n=2&delay=5'));
    let joined = '';
    for await (const chunk of res) joined += chunk;
    strictEqual(joined, 'chunk-0|chunk-1|');
    strictEqual(res.complete, true);
  },
};
