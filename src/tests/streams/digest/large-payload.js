// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Large chunks hash correctly without buffering issues.

import { deepStrictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';

export const largeChunksDigest = {
  async test() {
    const check = new Uint8Array([0x13, 0xb3, 0xf0, 0x58]);
    const digestStream = new crypto.DigestStream('crc32');
    const writer = digestStream.getWriter();
    await writer.write(Buffer.alloc(1024, 'a'));
    await writer.write(Buffer.alloc(1024, 'b'));
    await writer.write(Buffer.alloc(1024, 'c'));
    await writer.write(Buffer.alloc(1024 * 1024, 'd'));
    await writer.close();
    deepStrictEqual(new Uint8Array(await digestStream.digest), check);
  },
};
