// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Adapted streams meeting the runtime's own streams: fetch bodies in both
// directions, the workerd identity transforms, and standard transforms in a
// pipeThrough chain.
//
// FixedLengthStream length enforcement diverges (src/tests/streams/identity
// ledger #11): the C++ implementation errors the readable side with a
// TypeError while the offending write and the close succeed, so the node
// Writable adapted over the FixedLengthStream's writable never learns of it;
// the TypeScript implementation rejects the write or close eagerly with a
// RangeError, which the adapter surfaces as the node Writable's error, and
// the readable errors with the same RangeError.

import { Readable, Writable } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();
const dec = new TextDecoder();

function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

// A Readable that pushes asynchronously, with a small highWaterMark, feeds a
// Response whose text() yields the concatenation.
export const toWebAsResponseBody = {
  async test() {
    const r = new Readable({
      highWaterMark: 2,
      read() {},
    });
    setTimeout(() => r.push(enc.encode('ok')), 10);
    setTimeout(() => r.push(enc.encode(' there')), 20);
    setTimeout(() => r.push(null), 30);
    const res = new Response(Readable.toWeb(r));
    strictEqual(await res.text(), 'ok there');
  },
};

// The same stream serves as a Request body.
export const toWebAsRequestBody = {
  async test() {
    const r = new Readable({
      read() {
        this.push('req');
        this.push(' body');
        this.push(null);
      },
    });
    const request = new Request('http://example.com/', {
      method: 'POST',
      body: Readable.toWeb(r),
    });
    strictEqual(await request.text(), 'req body');
  },
};

// A megabyte pushed through Readable.toWeb into a Response arrives intact
// through arrayBuffer(); the consumer drives the source's backpressure.
export const toWebLargeResponseBody = {
  async test() {
    const CHUNK = 64 * 1024;
    const CHUNKS = 16;
    const r = new Readable({ read() {} });
    const collected = new Response(Readable.toWeb(r)).arrayBuffer();
    for (let i = 0; i < CHUNKS; i++) r.push(Buffer.alloc(CHUNK, i));
    r.push(null);
    const bytes = new Uint8Array(await collected);
    strictEqual(bytes.byteLength, CHUNK * CHUNKS);
    for (let i = 0; i < CHUNKS; i++) {
      strictEqual(bytes[i * CHUNK], i);
      strictEqual(bytes[(i + 1) * CHUNK - 1], i);
    }
  },
};

// A Response body — a runtime-provided (not JS-constructed) ReadableStream —
// adapts through Readable.fromWeb and yields Buffers.
export const fromWebResponseBody = {
  async test() {
    const chunks = [];
    for await (const chunk of Readable.fromWeb(
      new Response('resp body').body
    )) {
      chunks.push(chunk);
    }
    strictEqual(Buffer.isBuffer(chunks[0]), true);
    strictEqual(Buffer.concat(chunks).toString(), 'resp body');
  },
};

// A Response body piped through TextDecoderStream is a stream of strings;
// Readable.fromWeb in objectMode passes them through.
export const fromWebTextDecoderStreamBody = {
  async test() {
    const strings = [];
    const body = new Response('hé there').body.pipeThrough(
      new TextDecoderStream()
    );
    for await (const s of Readable.fromWeb(body, { objectMode: true })) {
      strings.push(s);
    }
    deepStrictEqual(strings, ['hé there']);
  },
};

// Node writes into an IdentityTransformStream's writable come out of its
// readable as a Response body.
export const fromWebIdentityTransformWritable = {
  async test() {
    const identity = new IdentityTransformStream();
    const w = Writable.fromWeb(identity.writable);
    const text = new Response(identity.readable).text();
    const finished = once(w, 'finish');
    w.write('hel');
    w.end('lo');
    strictEqual(await text, 'hello');
    await finished;
    strictEqual(w.writableFinished, true);
  },
};

// FixedLengthStream with exactly the promised bytes.
export const fromWebFixedLengthExact = {
  async test() {
    const fixed = new FixedLengthStream(5);
    const w = Writable.fromWeb(fixed.writable);
    const text = new Response(fixed.readable).text();
    const finished = once(w, 'finish');
    w.write('hel');
    w.end('lo');
    strictEqual(await text, 'hello');
    await finished;
    strictEqual(w.writableFinished, true);
  },
};

// Writing more than the promised length (ledger #11 shape per
// implementation).
export const fromWebFixedLengthOverwrite = {
  async test() {
    const fixed = new FixedLengthStream(3);
    const w = Writable.fromWeb(fixed.writable);
    const text = new Response(fixed.readable).text();
    const message =
      'Attempt to write too many bytes through a FixedLengthStream.';
    if (usingTsImpl) {
      const errored = once(w, 'error');
      const written = new Promise((resolve) =>
        w.write(Buffer.from('abcd'), resolve)
      );
      const err = await written;
      strictEqual(err.name, 'RangeError');
      strictEqual(err.message, message);
      strictEqual(await errored, err);
      await rejects(text, { name: 'RangeError', message });
      strictEqual(w.destroyed, true);
    } else {
      const errors = [];
      w.on('error', (err) => errors.push(err));
      const err = await new Promise((resolve) =>
        w.write(Buffer.from('abcd'), resolve)
      );
      strictEqual(err, undefined);
      await rejects(text, { name: 'TypeError', message });
      await scheduler.wait(5);
      deepStrictEqual(errors, []);
      strictEqual(w.destroyed, false);
    }
  },
};

// Ending before the promised length was delivered.
export const fromWebFixedLengthUnderwrite = {
  async test() {
    const fixed = new FixedLengthStream(5);
    const w = Writable.fromWeb(fixed.writable);
    const text = new Response(fixed.readable).text();
    const message =
      'FixedLengthStream did not see all expected bytes before close().';
    if (usingTsImpl) {
      const errored = once(w, 'error');
      w.end('hi');
      const err = await errored;
      strictEqual(err.name, 'RangeError');
      strictEqual(err.message, message);
      await rejects(text, { name: 'RangeError', message });
      strictEqual(w.writableFinished, false);
    } else {
      const finished = once(w, 'finish');
      w.end('hi');
      await finished;
      await rejects(text, { name: 'TypeError', message });
      strictEqual(w.writableFinished, true);
    }
  },
};

// A Readable.toWeb stream through a standard TransformStream into a web
// sink; and the reverse, a web source through a transform into
// Writable.fromWeb.
export const adaptersInPipeThroughChains = {
  async test() {
    const upper = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(dec.decode(chunk).toUpperCase());
      },
    });
    const out = [];
    const source = new Readable({
      read() {
        this.push('ab');
        this.push('cd');
        this.push(null);
      },
    });
    await Readable.toWeb(source)
      .pipeThrough(upper)
      .pipeTo(
        new WritableStream({
          write(chunk) {
            out.push(chunk);
          },
        })
      );
    strictEqual(out.join(''), 'ABCD');

    const doubled = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(chunk);
        controller.enqueue(chunk);
      },
    });
    const seen = [];
    const sink = Writable.fromWeb(
      new WritableStream({
        write(chunk) {
          seen.push(dec.decode(chunk));
        },
      })
    );
    const finished = once(sink, 'finish');
    for await (const chunk of Readable.fromWeb(
      new Response('x').body.pipeThrough(doubled)
    )) {
      sink.write(chunk);
    }
    sink.end();
    await finished;
    deepStrictEqual(seen, ['x', 'x']);
  },
};
