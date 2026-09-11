// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Readable.toWeb(): a node Readable driving a web ReadableStream. The
// adapter subscribes to 'data' and enqueues into the stream's controller,
// pausing the source whenever desiredSize drops to zero and resuming from
// pull().

import { Readable } from 'node:stream';
import { strictEqual } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

// A chunk pushed by the source arrives at the web reader.
export const toWebDeliversPushedChunk = {
  async test() {
    const r = new Readable({
      read() {
        this.push(enc.encode('ok'));
      },
    });
    const rs = Readable.toWeb(r);
    strictEqual(rs instanceof ReadableStream, true);
    const reader = rs.getReader();
    const { value, done } = await reader.read();
    strictEqual(done, false);
    strictEqual(dec.decode(value), 'ok');
  },
};
