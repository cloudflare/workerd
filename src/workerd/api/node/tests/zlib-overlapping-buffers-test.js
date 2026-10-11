// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// CompressionStream::write[Sync] accepted input and output ranges that alias the same memory.
// Brotli's fast encoder compresses directly into the output while still reading the input, so an
// overlapping output corrupted input the encoder had already examined.
import { ok, rejects, throws } from 'node:assert';
import zlib from 'node:zlib';

const N = 1 << 16;
const BROTLI_OPERATION_FINISH = 2;

function createHandles() {
  return [
    zlib.createBrotliCompress({
      params: { [zlib.constants.BROTLI_PARAM_QUALITY]: 1 },
    })._handle,
    zlib.createBrotliDecompress()._handle,
    zlib.createDeflate()._handle,
    zlib.createZstdCompress()._handle,
  ];
}

const overlapError = {
  name: 'Error',
  message: 'Input and output buffers must not overlap',
};

export const overlappingWriteSyncThrows = {
  test() {
    for (const handle of createHandles()) {
      const buf = new Uint8Array(N * 4);
      // Same region, output after input, and output before input.
      for (const [inOff, outOff] of [
        [0, 0],
        [0, N],
        [N, 0],
      ]) {
        throws(
          () => handle.writeSync(BROTLI_OPERATION_FINISH, buf, inOff, 2 * N, buf, outOff, 2 * N),
          overlapError
        );
      }
    }
  },
};

export const overlappingWriteThrows = {
  async test() {
    for (const handle of createHandles()) {
      const buf = new Uint8Array(N * 2);
      await rejects(
        async () => handle.write(BROTLI_OPERATION_FINISH, buf, 0, N, buf, N / 2, N),
        overlapError
      );
    }
  },
};

export const overlappingViewsOfOneArrayBufferThrow = {
  test() {
    const [handle] = createHandles();
    const ab = new ArrayBuffer(N * 2);
    const input = new Uint8Array(ab, 0, N + 1);
    const output = new Uint8Array(ab, N);
    throws(
      () => handle.writeSync(BROTLI_OPERATION_FINISH, input, 0, N + 1, output, 0, N),
      overlapError
    );
  },
};

export const adjacentAndEmptyRangesAreAllowed = {
  test() {
    const [handle] = createHandles();
    const buf = new Uint8Array(N * 4);
    // Adjacent, non-overlapping halves of one buffer are fine.
    handle.writeSync(0, buf, 0, N, buf, N, 3 * N);
    // An empty input inside the output range does not overlap anything.
    handle.writeSync(BROTLI_OPERATION_FINISH, buf, N * 2, 0, buf, 0, N * 4);
    ok(true);
  },
};
