// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The digest promise and bytesWritten counter.

import { strictEqual } from 'node:assert';

export const digestPromiseIdentityIsStable = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const first = stream.digest;
    strictEqual(stream.digest, first, 'digest must be the same promise object');

    const writer = stream.getWriter();
    await writer.close();
    await first;
    // Still the same object after settling.
    strictEqual(stream.digest, first);
  },
};

export const bytesWrittenIsBigInt = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    strictEqual(typeof stream.bytesWritten, 'bigint');
    strictEqual(stream.bytesWritten, 0n);

    const writer = stream.getWriter();
    await writer.write(new Uint8Array(3));
    strictEqual(stream.bytesWritten, 3n);
    // A zero-length write must not move the counter.
    await writer.write(new Uint8Array(0));
    strictEqual(stream.bytesWritten, 3n);
    // Strings count their UTF-8 length, not their UTF-16 length.
    await writer.write('\u00e9');
    strictEqual(stream.bytesWritten, 5n);
    await writer.close();
  },
};
