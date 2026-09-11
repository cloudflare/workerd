// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Duplex.toWeb(): a { readable, writable } pair over a node Duplex, built
// from the Readable.toWeb and Writable.toWeb adapters over its two halves.

import { Duplex } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual } from 'node:assert';

// Writes through the web writer reach the duplex's _write(); pushes from
// its _read() arrive at the web reader.
export const toWebPairRoundTrip = {
  async test() {
    const dataToRead = Buffer.from('hello');
    const dataToWrite = Buffer.from('world');
    const written = Promise.withResolvers();

    const duplex = new Duplex({
      read() {
        this.push(dataToRead);
        this.push(null);
      },
      write(chunk, encoding, callback) {
        strictEqual(chunk, dataToWrite);
        written.resolve();
        callback();
      },
    });

    const { writable, readable } = Duplex.toWeb(duplex);
    strictEqual(writable instanceof WritableStream, true);
    strictEqual(readable instanceof ReadableStream, true);

    const writer = writable.getWriter();
    const [, result] = await Promise.all([
      writer.write(dataToWrite),
      readable.getReader().read(),
      written.promise,
    ]);
    strictEqual(result.done, false);
    deepStrictEqual(Buffer.from(result.value), dataToRead);
  },
};
