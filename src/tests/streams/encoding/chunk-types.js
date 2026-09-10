// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Chunk validation. TextDecoderStream accepts BufferSource only; the
// rejection TypeError's message diverges (pinned below) but the aftermath
// is shared: the stream errors and every later interaction rejects with
// the same error. TextEncoderStream ToString-coerces, so symbols throw.

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
