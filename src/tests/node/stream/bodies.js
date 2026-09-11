// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Adapted streams as fetch bodies: the ReadableStream produced by
// Readable.toWeb() is a valid Response body, consumed by the runtime's body
// machinery rather than a JS reader.

import { Readable } from 'node:stream';
import { strictEqual } from 'node:assert';

const enc = new TextEncoder();

// A Readable that pushes asynchronously, with a small highWaterMark, feeds a
// Response whose text() yields the concatenation.
export const toWebAsResponseBody = {
  async test() {
    const r = new Readable({
      highWaterMark: 2,
      read() {},
    });
    setTimeout(() => r.push(enc.encode('ok')), 10);
    setTimeout(() => r.push(enc.encode(' there')), 20);
    setTimeout(() => r.push(null), 30);
    const res = new Response(Readable.toWeb(r));
    strictEqual(await res.text(), 'ok there');
  },
};
