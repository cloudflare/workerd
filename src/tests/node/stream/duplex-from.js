// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Duplex.from() over a lone web stream: the result is a Duplex whose
// missing side is marked finished (writable: false) or ended (readable:
// false), so that ordinary consumption of the present side leaves nothing
// unfinished behind.

import { Duplex } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

async function collect(readable) {
  const chunks = [];
  for await (const chunk of readable) chunks.push(chunk);
  return Buffer.concat(chunks).toString();
}

// Duplex.from() over a lone web stream marks the missing side finished, so
// consuming the readable-only result to completion (which destroys it) is
// clean, and ending the writable-only result finishes it.
export const duplexFromWebStreamHalves = {
  async test() {
    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('only-readable'));
        controller.close();
      },
    });
    const readable = Duplex.from(source);
    const errors = [];
    readable.on('error', (err) => errors.push(err));
    strictEqual(readable.writableFinished, true);
    strictEqual(await collect(readable), 'only-readable');
    strictEqual(readable.destroyed, true);
    deepStrictEqual(errors, []);

    const seen = [];
    const writable = Duplex.from(
      new WritableStream({
        write(chunk) {
          seen.push(dec.decode(chunk));
        },
      })
    );
    strictEqual(writable.readableEnded, true);
    await new Promise((resolve) => writable.end('only-writable', resolve));
    strictEqual(writable.writableFinished, true);
    deepStrictEqual(seen, ['only-writable']);
  },
};
