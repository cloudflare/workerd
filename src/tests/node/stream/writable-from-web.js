// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Writable.fromWeb(): a node Writable forwarding to a web WritableStream.
// The adapter takes a writer at construction; each _write() awaits
// writer.ready then writer.write(), _final() calls writer.close(), and
// _destroy() aborts (with an error) or closes (without one).

import { Writable } from 'node:stream';
import { strictEqual } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

// A chunk written to the node Writable reaches the web sink's write().
export const fromWebWritesReachWebSink = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const ws = new WritableStream({
      write(chunk) {
        strictEqual(dec.decode(chunk), 'ok');
        resolve();
      },
    });
    const w = Writable.fromWeb(ws);
    strictEqual(w instanceof Writable, true);
    const written = Promise.withResolvers();
    w.write(enc.encode('ok'), (err) => {
      strictEqual(err, undefined);
      written.resolve();
    });
    await Promise.all([promise, written.promise]);
  },
};
