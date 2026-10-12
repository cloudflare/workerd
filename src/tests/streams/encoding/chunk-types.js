// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Chunk validation. TextDecoderStream accepts BufferSource only; the
// rejection TypeError's message diverges (pinned below) but the aftermath
// is shared: the stream errors and every later interaction rejects with
// the same error. TextEncoderStream ToString-coerces, so symbols throw.
//
// A bare SharedArrayBuffer chunk diverges: TypeScript decodes it, as the
// Encoding Standard's AllowSharedBufferSource writable (and Node) do; C++
// rejects it as not a BufferSource. Views over shared memory decode in
// both.

import { strictEqual, deepStrictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const cppBadChunkMsg =
  'This TransformStream is being used as a byte stream, but received a ' +
  'value that is not a BufferSource.';
const tsBadChunkMsg = 'TextDecoderStream: chunk must be a BufferSource';

export const decoderAcceptsBufferSources = {
  async test() {
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const reader = tds.readable.getReader();
    const enc = new TextEncoder();

    // 'B' and 'C' delivered through a larger buffer via view offsets.
    const backing = new Uint8Array([0x00, 0x42, 0x43, 0x00]);
    const chunks = [
      enc.encode('A').buffer, // ArrayBuffer
      backing.subarray(1, 2), // Uint8Array view
      new DataView(backing.buffer, 2, 1), // DataView subrange
    ];
    const got = [];
    for (const chunk of chunks) {
      const [, result] = await Promise.all([
        writer.write(chunk),
        reader.read(),
      ]);
      got.push(result.value);
    }
    deepStrictEqual(got, ['A', 'B', 'C']);
    await writer.close();
  },
};

export const decoderSharedArrayBufferChunks = {
  async test() {
    const shared = (bytes) => {
      const buffer = new SharedArrayBuffer(bytes.length);
      new Uint8Array(buffer).set(bytes);
      return buffer;
    };
    const decode = async (chunks) => {
      const tds = new TextDecoderStream();
      const writer = tds.writable.getWriter();
      const reader = tds.readable.getReader();
      const writes = Promise.all([
        ...chunks.map((c) => writer.write(c)),
        writer.close(),
      ]);
      let text = '';
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        text += value;
      }
      await writes;
      return text;
    };
    // Views over shared memory, including one split character: parity.
    const euro = [0xe2, 0x82, 0xac];
    strictEqual(
      await decode([
        new Uint8Array(shared([0x00, 0x41, 0x00]), 1, 1),
        new DataView(shared([0x42])),
        new Uint8Array(shared(euro.slice(0, 1))),
        new Uint8Array(shared(euro.slice(1))),
      ]),
      'AB€'
    );
    // A bare SharedArrayBuffer (also growable, and a character split across
    // two of them): TypeScript decodes it; C++ rejects it as an invalid
    // chunk (ledger #8).
    const growable = new SharedArrayBuffer(1, { maxByteLength: 4 });
    new Uint8Array(growable)[0] = 0x44;
    const bare = [
      shared([0x43]),
      growable,
      shared(euro.slice(0, 2)),
      shared(euro.slice(2)),
    ];
    if (usingTsImpl) {
      strictEqual(await decode(bare), 'CD€');
    } else {
      await rejects(decode(bare), (err) => {
        strictEqual(err.constructor, TypeError);
        strictEqual(err.message, cppBadChunkMsg);
        return true;
      });
    }
  },
};

export const decoderDetachedBufferIsNoop = {
  async test() {
    // An already-detached ArrayBuffer decodes as zero bytes: the write and
    // close resolve and the reader sees clean EOF.
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const reader = tds.readable.getReader();
    const ab = new ArrayBuffer(1);
    new Uint8Array(ab)[0] = 0x43;
    ab.transfer();
    const readPromise = reader.read();
    await Promise.all([writer.write(ab), writer.close()]);
    strictEqual((await readPromise).done, true);
  },
};

export const decoderRejectsNonBufferSource = {
  async test() {
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const reader = tds.readable.getReader();
    const readPromise = reader.read();
    const expectedMsg = usingTsImpl ? tsBadChunkMsg : cppBadChunkMsg;
    const check = (err) => {
      strictEqual(err.constructor, TypeError);
      strictEqual(err.message, expectedMsg);
      return true;
    };
    await rejects(writer.write(42), check);
    // The stream is errored: every side rejects with the same error.
    await rejects(writer.closed, check);
    await rejects(readPromise, check);
    await rejects(writer.write(new Uint8Array([0x41])), check);
  },
};

export const encoderSymbolChunkErrorsStream = {
  async test() {
    // ToString(symbol) throws; the TypeError errors both sides.
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    const reader = tes.readable.getReader();
    const readPromise = reader.read();
    const check = (err) => {
      strictEqual(err.constructor, TypeError);
      strictEqual(err.message, 'Cannot convert a Symbol value to a string');
      return true;
    };
    await rejects(writer.write(Symbol('x')), check);
    await rejects(writer.closed, check);
    await rejects(readPromise, check);
  },
};
