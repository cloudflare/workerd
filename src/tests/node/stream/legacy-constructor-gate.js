// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without streams_enable_constructors the C++ implementation refuses
// `new ReadableStream()` / `new WritableStream()`, and the toWeb adapters,
// which construct their streams that way, fail with that Error. The
// fromWeb adapters construct nothing and keep working over streams the
// runtime provides (fetch bodies, the identity transforms), as does
// pipeline over such streams. Without
// transformstream_enable_standard_constructor, `new TransformStream()`
// still constructs — as an identity transform that ignores its transformer.

import { Readable, Writable, Duplex, PassThrough, pipeline } from 'node:stream';
import * as web from 'node:stream/web';
import { Buffer } from 'node:buffer';
import { strictEqual, throws } from 'node:assert';

const gateError = (cls) => ({
  name: 'Error',
  message: new RegExp(
    `^To use the new ${cls}\\(\\) constructor, enable the streams_enable_constructors compatibility flag\\.`
  ),
});

// Every toWeb entry point hits the gate, including the paths for
// destroyed/unreadable inputs, which construct a stream to pre-cancel or
// pre-close.
export const legacyToWebHitsConstructorGate = {
  async test() {
    throws(
      () => Readable.toWeb(new Readable({ read() {} })),
      gateError('ReadableStream')
    );
    throws(
      () => Writable.toWeb(new Writable({ write() {} })),
      gateError('WritableStream')
    );
    // Duplex.toWeb builds the writable half first.
    throws(() => Duplex.toWeb(new PassThrough()), gateError('WritableStream'));

    const destroyed = new Readable({ read() {} });
    destroyed.destroy();
    await new Promise((resolve) => destroyed.once('close', resolve));
    throws(() => Readable.toWeb(destroyed), gateError('ReadableStream'));
    throws(
      () => Readable.toWeb(new Duplex({ readable: false, write() {} })),
      gateError('ReadableStream')
    );
  },
};

// The node:stream/web re-exports are the gated globals.
export const legacyStreamWebConstructorsGated = {
  test() {
    throws(() => new web.ReadableStream(), gateError('ReadableStream'));
    throws(() => new web.WritableStream(), gateError('WritableStream'));
    strictEqual(web.ReadableStream, globalThis.ReadableStream);
  },
};

// fromWeb over runtime-provided streams needs no constructor.
export const legacyFromWebOverRuntimeStreams = {
  async test() {
    const chunks = [];
    for await (const chunk of Readable.fromWeb(new Response('body').body)) {
      chunks.push(chunk);
    }
    strictEqual(Buffer.concat(chunks).toString(), 'body');

    const identity = new IdentityTransformStream();
    const w = Writable.fromWeb(identity.writable);
    const text = new Response(identity.readable).text();
    w.end('through identity');
    strictEqual(await text, 'through identity');

    const pair = new IdentityTransformStream();
    const duplex = Duplex.fromWeb({
      readable: new Response('in').body,
      writable: pair.writable,
    });
    const out = new Response(pair.readable).text();
    const read = [];
    duplex.on('data', (chunk) => read.push(chunk));
    duplex.end('out');
    await new Promise((resolve) => duplex.once('end', resolve));
    strictEqual(Buffer.concat(read).toString(), 'in');
    strictEqual(await out, 'out');
  },
};

// pipeline() with runtime-provided web streams works too.
export const legacyPipelineOverRuntimeStreams = {
  async test() {
    const sinkChunks = [];
    const sink = new Writable({
      write(chunk, encoding, callback) {
        sinkChunks.push(chunk);
        callback();
      },
    });
    await new Promise((resolve) =>
      pipeline(new Response('piped').body, sink, resolve)
    );
    strictEqual(Buffer.concat(sinkChunks).toString(), 'piped');

    const identity = new IdentityTransformStream();
    const text = new Response(identity.readable).text();
    await new Promise((resolve) =>
      pipeline(
        Readable.from([Buffer.from('to identity')]),
        identity.writable,
        resolve
      )
    );
    strictEqual(await text, 'to identity');
  },
};

// Without transformstream_enable_standard_constructor a TransformStream is
// an identity transform: the transformer passed to it is never consulted.
export const legacyTransformStreamIgnoresTransformer = {
  async test() {
    let transformCalls = 0;
    const transform = new TransformStream({
      transform(chunk, controller) {
        transformCalls++;
        controller.enqueue(new TextEncoder().encode('TRANSFORMED'));
      },
    });
    const sinkChunks = [];
    const sink = new Writable({
      write(chunk, encoding, callback) {
        sinkChunks.push(chunk);
        callback();
      },
    });
    await new Promise((resolve) =>
      pipeline(
        Readable.from([Buffer.from('as-is')], { objectMode: false }),
        transform,
        sink,
        resolve
      )
    );
    strictEqual(transformCalls, 0);
    strictEqual(Buffer.concat(sinkChunks).toString(), 'as-is');
  },
};
