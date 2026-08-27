// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without strict_compression_checks, DecompressionStream tolerates trailing
// bytes after the compressed member (delivering the member's content),
// close() with no data, and close() mid-member. Without
// capture_async_api_throws, an invalid chunk throws synchronously and the
// stream survives.

import { strictEqual, throws } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

async function drainText(readable) {
  let out = '';
  for await (const chunk of readable) {
    out += dec.decode(chunk, { stream: true });
  }
  return out + dec.decode();
}

export const legacyTrailingDataTolerated = {
  async test() {
    // Gzipped "FOOBAR" plus a trailing 0xFF byte.
    const trailing = new Uint8Array([
      0x1f, 0x8b, 0x08, 0x00, 0xf9, 0x05, 0xb7, 0x59, 0x00, 0x03, 0x4b,
      0xcb, 0xcf, 0x4f, 0x4a, 0x2c, 0x02, 0x00, 0x95, 0x1f, 0xf6, 0x9e,
      0x06, 0x00, 0x00, 0x00, 0xff,
    ]);
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    await writer.write(trailing);
    await writer.close();
    strictEqual(await drainText(ds.readable), 'foobar');
  },
};

export const legacyCloseWithoutDataTolerated = {
  async test() {
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    await writer.close();
    strictEqual(await drainText(ds.readable), '');
  },
};

export const legacyTruncatedMemberCloseTolerated = {
  async test() {
    const cs = new CompressionStream('gzip');
    const cw = cs.writable.getWriter();
    await cw.write(enc.encode('hello world'));
    await cw.close();
    const bytes = [];
    for await (const chunk of cs.readable) {
      bytes.push(...chunk);
    }
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    await writer.write(new Uint8Array(bytes.slice(0, bytes.length - 4)));
    await writer.close();
  },
};

export const legacyInvalidChunkThrowsSynchronously = {
  async test() {
    const cs = new CompressionStream('deflate');
    const writer = cs.writable.getWriter();
    throws(() => writer.write(42), (err) => {
      strictEqual(err.constructor, TypeError);
      strictEqual(
        err.message,
        'This TransformStream is being used as a byte stream, but received ' +
          'an object of non-ArrayBuffer/ArrayBufferView type on its ' +
          'writable side.'
      );
      return true;
    });
    // The stream survives: later traffic flows.
    await writer.write(enc.encode('ok'));
    await writer.close();
  },
};
