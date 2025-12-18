// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects } from 'node:assert';

// Test strictCompression flag: closing without finishing decompression should error
export const strictCompressionCloseWithoutFinish = {
  async test() {
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    // Closing without writing any data should fail because gzip expects proper framing
    await rejects(writer.close(), TypeError);
  },
};

// Test strictCompression flag: trailing data after valid gzip stream should error
export const strictCompressionTrailingData = {
  async test() {
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();

    // Gzipped string "FOOBAR", plus a trailing 0xFF byte
    const trailingStrm = new Uint8Array([
      0x1f, 0x8b, 0x08, 0x00, 0xf9, 0x05, 0xb7, 0x59, 0x00, 0x03, 0x4b, 0xcb,
      0xcf, 0x4f, 0x4a, 0x2c, 0x02, 0x00, 0x95, 0x1f, 0xf6, 0x9e, 0x06, 0x00,
      0x00, 0x00, 0xff,
    ]);

    await rejects(writer.write(trailingStrm), TypeError);
  },
};
