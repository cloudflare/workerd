// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Chunk validation: ArrayBuffer, any ArrayBufferView (offsets honored),
// and strings are accepted; everything else — including a bare
// SharedArrayBuffer — rejects with the same TypeError in both
// implementations. Views onto SharedArrayBuffers are accepted.

import { strictEqual, deepStrictEqual, rejects } from 'node:assert';

const badChunkMsg =
  'DigestStream is a byte stream but received an object ' +
  'of non-ArrayBuffer/ArrayBufferView/string type on its writable side.';

export const rejectsNonByteChunks = {
  async test() {
    for (const bad of [123, null, undefined, {}, [], true, Symbol.iterator]) {
      const stream = new crypto.DigestStream('md5');
      const writer = stream.getWriter();
      await rejects(writer.write(bad), { message: badChunkMsg });
    }
  },
};

export const sharedArrayBufferHandling = {
  async test() {
    {
      const stream = new crypto.DigestStream('md5');
      const writer = stream.getWriter();
      await rejects(writer.write(new SharedArrayBuffer(8)), {
        message: badChunkMsg,
      });
    }
    {
      const stream = new crypto.DigestStream('md5');
      const writer = stream.getWriter();
      await writer.write(new Uint8Array(new SharedArrayBuffer(4)));
      await writer.close();
      await stream.digest;
      strictEqual(stream.bytesWritten, 4n);
    }
  },
};

export const dataViewRespectsOffset = {
  async test() {
    const backing = new Uint8Array([9, 9, 1, 2, 3, 4, 9, 9]);
    const stream = new crypto.DigestStream('crc32');
    const writer = stream.getWriter();
    await writer.write(new DataView(backing.buffer, 2, 4));
    await writer.close();

    const reference = new crypto.DigestStream('crc32');
    const refWriter = reference.getWriter();
    await refWriter.write(new Uint8Array([1, 2, 3, 4]));
    await refWriter.close();

    deepStrictEqual(
      new Uint8Array(await stream.digest),
      new Uint8Array(await reference.digest)
    );
    strictEqual(stream.bytesWritten, 4n);
  },
};
