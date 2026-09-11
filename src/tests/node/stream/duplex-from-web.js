// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Duplex.fromWeb(): a node Duplex over a { readable, writable } pair of web
// streams, taking a reader and a writer at construction.

import { Duplex } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual } from 'node:assert';

// Data written to the duplex reaches the web writable's sink; chunks from
// the web readable surface as 'data'.
export const fromWebPairRoundTrip = {
  async test() {
    const dataToRead = Buffer.from('hello');
    const dataToWrite = Buffer.from('world');
    const sinkWrote = Promise.withResolvers();

    const readable = new ReadableStream({
      start(controller) {
        controller.enqueue(dataToRead);
      },
    });
    const writable = new WritableStream({
      write(chunk) {
        strictEqual(chunk, dataToWrite);
        sinkWrote.resolve();
      },
    });

    const duplex = Duplex.fromWeb({ readable, writable });
    strictEqual(duplex instanceof Duplex, true);

    duplex.write(dataToWrite);
    const read = Promise.withResolvers();
    duplex.once('data', (chunk) => {
      strictEqual(chunk, dataToRead);
      read.resolve();
    });
    await Promise.all([read.promise, sinkWrote.promise]);
  },
};

// With objectMode and an encoding, string chunks pass through both
// directions untouched.
export const fromWebObjectModeStrings = {
  async test() {
    const dataToRead = 'hello';
    const dataToWrite = 'world';
    const sinkWrote = Promise.withResolvers();

    const readable = new ReadableStream({
      start(controller) {
        controller.enqueue(dataToRead);
      },
    });
    const writable = new WritableStream({
      write(chunk) {
        strictEqual(chunk, dataToWrite);
        sinkWrote.resolve();
      },
    });

    const duplex = Duplex.fromWeb(
      { readable, writable },
      { encoding: 'utf8', objectMode: true }
    );

    duplex.write(dataToWrite);
    const read = Promise.withResolvers();
    duplex.once('data', (chunk) => {
      strictEqual(chunk, dataToRead);
      read.resolve();
    });
    await Promise.all([read.promise, sinkWrote.promise]);
  },
};

// Corked writes are batched through _writev() and reach the web sink as
// the chunks themselves, in order.
export const fromWebPairCorkedWritesDeliverChunks = {
  async test() {
    const seen = [];
    const duplex = Duplex.fromWeb({
      readable: new ReadableStream(),
      writable: new WritableStream({
        write(chunk) {
          seen.push(chunk);
        },
      }),
    });
    duplex.cork();
    duplex.write('a');
    duplex.write('b');
    duplex.write('c');
    duplex.uncork();
    await new Promise((resolve) => duplex.end(resolve));
    strictEqual(seen.length, 3);
    for (const chunk of seen) {
      strictEqual(chunk instanceof Uint8Array, true);
    }
    strictEqual(Buffer.concat(seen).toString(), 'abc');
  },
};
