// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Chunk validation. Divergences:
// - Strings: the C++ internal writable accepts and UTF-8 encodes them (the
//   reason WPT's compression-bad-chunks is an expected failure there);
//   TypeScript rejects them per spec.
// - SharedArrayBuffer and SAB-backed views: C++ accepts (copies out);
//   TypeScript rejects per Web IDL (no [AllowShared] here).
// - Invalid-chunk aftermath: the C++ stream SURVIVES a rejected chunk
//   (later writes flow, close is clean); TypeScript errors BOTH sides.
// Parity: ArrayBuffer, DataView, other views (offsets honored), empty and
// already-detached inputs are accepted by both.

import { strictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { pump } from 'round-trip';

const enc = new TextEncoder();
const dec = new TextDecoder();

const cppBadChunkMsg =
  'This TransformStream is being used as a byte stream, but received an ' +
  'object of non-ArrayBuffer/ArrayBufferView type on its writable side.';
const tsBadChunkMsg =
  'The provided value is not of type (ArrayBuffer or ArrayBufferView)';

async function roundTrip(chunks) {
  const compressed = await pump(new CompressionStream('gzip'), chunks);
  return dec.decode(await pump(new DecompressionStream('gzip'), [compressed]));
}

export const bufferSourceChunksAccepted = {
  async test() {
    // 'B' and 'C' arrive through views over a larger buffer.
    const backing = new Uint8Array([0x00, 0x42, 0x43, 0x00]);
    strictEqual(
      await roundTrip([
        enc.encode('A').buffer, // ArrayBuffer
        backing.subarray(1, 2), // Uint8Array view
        new DataView(backing.buffer, 2, 1), // DataView subrange
      ]),
      'ABC'
    );
  },
};

export const stringChunkDiverges = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    if (usingTsImpl) {
      // Rejected — but per-write only (the DECIDED invalid-chunk
      // contract): the stream survives and later traffic flows.
      await rejects(writer.write('hi'), (err) => {
        strictEqual(err.constructor, TypeError);
        strictEqual(err.message, tsBadChunkMsg);
        return true;
      });
      await writer.write(enc.encode('hi'));
      await writer.close();
      const chunks = [];
      for await (const chunk of cs.readable) {
        chunks.push(chunk);
      }
      strictEqual(
        dec.decode(await pump(new DecompressionStream('gzip'), chunks)),
        'hi'
      );
    } else {
      // Accepted and UTF-8 encoded, like the identity streams.
      await writer.write('hi');
      await writer.close();
      const chunks = [];
      for await (const chunk of cs.readable) {
        chunks.push(chunk);
      }
      strictEqual(
        dec.decode(await pump(new DecompressionStream('gzip'), chunks)),
        'hi'
      );
    }
  },
};

// SAB-backed chunks are accepted by copying the shared bytes (parity —
// DECIDED 2026-08-28, matching the identity streams and C++; the strict
// [AllowShared]-less BufferSource reading was considered and overridden,
// which is why the WPT bad-chunks files are disabled for both impls).
export const sharedArrayBufferChunkAccepted = {
  async test() {
    const sab = new SharedArrayBuffer(1);
    new Uint8Array(sab)[0] = 0x41;
    for (const chunk of [sab, new Uint8Array(sab)]) {
      const cs = new CompressionStream('gzip');
      const writer = cs.writable.getWriter();
      await writer.write(chunk);
      await writer.close();
      const chunks = [];
      for await (const c of cs.readable) {
        chunks.push(c);
      }
      strictEqual(
        dec.decode(await pump(new DecompressionStream('gzip'), chunks)),
        'A'
      );
      // The shared bytes were copied, never consumed in place.
      strictEqual(new Uint8Array(sab)[0], 0x41);
    }
  },
};

// An invalid chunk rejects ITS OWN write only (message per impl); the
// stream survives on both sides (parity — the DECIDED contract).
export const invalidChunkRejectsWriteOnly = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    const expectedMsg = usingTsImpl ? tsBadChunkMsg : cppBadChunkMsg;
    await rejects(writer.write(42), (err) => {
      strictEqual(err.constructor, TypeError);
      strictEqual(err.message, expectedMsg);
      return true;
    });
    // The stream survives: later traffic flows and close is clean.
    await writer.write(enc.encode('ok'));
    await writer.close();
    const chunks = [];
    for await (const chunk of cs.readable) {
      chunks.push(chunk);
    }
    strictEqual(
      dec.decode(await pump(new DecompressionStream('gzip'), chunks)),
      'ok'
    );
  },
};
