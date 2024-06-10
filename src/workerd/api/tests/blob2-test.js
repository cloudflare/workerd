// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import {
  ok,
  strictEqual,
  deepStrictEqual,
} from 'node:assert';

export const test = {
  async test() {
    const res = new Response('test', {
      headers: {
        'content-type': 'text/plain, x/y, text/html',
      }
    });

    const blob = await res.blob();

    // Without the blob_standard_mime_type compat flag, the blob.type would be
    // the incorrect 'text/plain, x/y, text/html'

    strictEqual(await blob.text(), 'test');
    strictEqual(blob.type, 'text/html');
  }
};

export const bytes = {
  async test() {
    const check = new Uint8Array([116, 101, 115, 116]);
    const blob = new Blob(['test']);
    const u8 = await blob.bytes();
    deepStrictEqual(u8, check);
    ok(u8 instanceof Uint8Array);

    const res = new Response('test');
    const u8_2 = await res.bytes();
    deepStrictEqual(u8_2, check);
    ok(u8_2 instanceof Uint8Array);
  }
};
