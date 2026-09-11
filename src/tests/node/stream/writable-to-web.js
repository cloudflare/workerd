// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Writable.toWeb(): a web WritableStream whose sink forwards to a node
// Writable. Sink writes call the node write(), sink close() calls end(),
// sink abort() destroys, and a node-side error or 'drain' settles the
// corresponding web promises.

import { Writable } from 'node:stream';
import { strictEqual } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

// A chunk written through the web writer reaches the node _write().
export const toWebWritesReachNodeSink = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const w = new Writable({
      write(chunk, encoding, callback) {
        strictEqual(dec.decode(chunk), 'ok');
        resolve();
        callback();
      },
    });
    const ws = Writable.toWeb(w);
    strictEqual(ws instanceof WritableStream, true);
    const writer = ws.getWriter();
    await Promise.all([writer.write(enc.encode('ok')), promise]);
  },
};
