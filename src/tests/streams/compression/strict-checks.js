// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// strict_compression_checks (pinned in both cells): DecompressionStream
// rejects trailing bytes after the compressed member at write time, and
// rejects close() while the member is incomplete.

import { rejects } from 'node:assert';
import { pump } from 'round-trip';

// Gzipped "FOOBAR" plus a trailing 0xFF byte.
const trailingGzip = new Uint8Array([
  0x1f, 0x8b, 0x08, 0x00, 0xf9, 0x05, 0xb7, 0x59, 0x00, 0x03, 0x4b, 0xcb, 0xcf,
  0x4f, 0x4a, 0x2c, 0x02, 0x00, 0x95, 0x1f, 0xf6, 0x9e, 0x06, 0x00, 0x00, 0x00,
  0xff,
]);

export const trailingDataRejectsWrite = {
  async test() {
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    await rejects(writer.write(trailingGzip), TypeError);
  },
};

export const closeWithoutAnyDataRejects = {
  async test() {
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    await rejects(writer.close(), TypeError);
  },
};

export const truncatedMemberRejectsClose = {
  async test() {
    // Every write succeeds (the bytes are valid so far); the incomplete
    // member is only detectable at the close-time flush.
    const compressed = await pump(new CompressionStream('gzip'), [
      new TextEncoder().encode('hello world'),
    ]);
    const truncated = compressed.slice(0, compressed.length - 4);
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    await writer.write(truncated);
    await rejects(writer.close(), TypeError);
  },
};
