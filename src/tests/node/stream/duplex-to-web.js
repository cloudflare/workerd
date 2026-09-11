// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Duplex.toWeb(): a { readable, writable } pair over a node Duplex, built
// from the Readable.toWeb and Writable.toWeb adapters over its two halves.

import { Duplex, Readable, Writable } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, rejects, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

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

// Anything without both a _readableState and a _writableState is rejected
// with ERR_INVALID_ARG_TYPE.
export const toWebRejectsNonDuplex = {
  test() {
    for (const input of [new Readable(), new Writable(), {}, null]) {
      throws(() => Duplex.toWeb(input), {
        name: 'TypeError',
        code: 'ERR_INVALID_ARG_TYPE',
        message: /"duplex" argument must be an stream\.Duplex/,
      });
    }
  },
};

// A destroyed Duplex yields a pair whose readable is cancelled and whose
// writable is closed.
export const toWebDestroyedDuplexYieldsClosedPair = {
  async test() {
    const duplex = new Duplex({ read() {}, write() {} });
    duplex.destroy();
    await new Promise((resolve) => duplex.once('close', resolve));
    const { readable, writable } = Duplex.toWeb(duplex);
    strictEqual((await readable.getReader().read()).done, true);
    await writable.getWriter().closed;
  },
};

// A Duplex without a readable side yields a cancelled readable while its
// writable half stays live, and vice versa.
export const toWebHalfDuplexes = {
  async test() {
    const writes = [];
    const writeOnly = new Duplex({
      readable: false,
      write(chunk, encoding, callback) {
        writes.push(chunk);
        callback();
      },
    });
    const writeOnlyPair = Duplex.toWeb(writeOnly);
    strictEqual((await writeOnlyPair.readable.getReader().read()).done, true);
    await writeOnlyPair.writable.getWriter().write(Buffer.from('w'));
    strictEqual(writes.length, 1);

    const readOnly = new Duplex({
      writable: false,
      read() {
        this.push(Buffer.from('r'));
        this.push(null);
      },
    });
    const readOnlyPair = Duplex.toWeb(readOnly);
    await readOnlyPair.writable.getWriter().closed;
    const reader = readOnlyPair.readable.getReader();
    strictEqual(Buffer.from((await reader.read()).value).toString(), 'r');
    strictEqual((await reader.read()).done, true);
  },
};

// The readable half is a default (non-byte) stream: a BYOB reader is
// refused. The refusal message differs between implementations.
export const toWebReadableIsNotByteStream = {
  test() {
    const { readable } = Duplex.toWeb(new Duplex({ read() {}, write() {} }));
    throws(() => readable.getReader({ mode: 'byob' }), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'BYOB reader can only be used on a stream with a byte source'
        : 'This ReadableStream does not support BYOB reads.',
    });
  },
};

// Destroying the Duplex with an error errors both halves with that error.
export const toWebDestroyWithErrorErrorsBothHalves = {
  async test() {
    const duplex = new Duplex({ read() {}, write() {} });
    duplex.on('error', () => {});
    const { readable, writable } = Duplex.toWeb(duplex);
    const reader = readable.getReader();
    const writer = writable.getWriter();
    const pending = reader.read();
    const boom = new Error('duplex boom');
    duplex.destroy(boom);
    await rejects(pending, (err) => err === boom);
    await rejects(reader.closed, (err) => err === boom);
    await rejects(writer.closed, (err) => err === boom);
  },
};

// Both halves observe the whole Duplex through end-of-stream, which for a
// Duplex means both of its sides. With allowHalfOpen (the Duplex default),
// closing the writable half finishes the node writable side and the readable
// half keeps flowing, but the close() promise settles only once the readable
// side has ended as well.
export const toWebClosingWritableWaitsForReadableEnd = {
  async test() {
    const duplex = new Duplex({
      read() {},
      write(chunk, encoding, callback) {
        callback();
      },
    });
    strictEqual(duplex.allowHalfOpen, true);
    const { readable, writable } = Duplex.toWeb(duplex);
    const writer = writable.getWriter();
    await writer.write(Buffer.from('in'));
    const finished = new Promise((resolve) => duplex.once('finish', resolve));
    let closeSettled = false;
    const closing = writer.close().then(() => {
      closeSettled = true;
    });
    await finished;
    strictEqual(duplex.writableFinished, true);
    await scheduler.wait(5);
    strictEqual(closeSettled, false);

    const reader = readable.getReader();
    duplex.push(Buffer.from('out'));
    strictEqual(Buffer.from((await reader.read()).value).toString(), 'out');
    duplex.push(null);
    strictEqual((await reader.read()).done, true);
    await closing;
    strictEqual(closeSettled, true);
  },
};

// Symmetrically, the readable half reports done only once the node writable
// side has finished too: a pushed EOF alone leaves the read pending until
// the writer is closed.
export const toWebReadableEofWaitsForWritableFinish = {
  async test() {
    const duplex = new Duplex({
      read() {},
      write(chunk, encoding, callback) {
        callback();
      },
    });
    const { readable, writable } = Duplex.toWeb(duplex);
    const reader = readable.getReader();
    duplex.push(Buffer.from('only'));
    strictEqual(Buffer.from((await reader.read()).value).toString(), 'only');
    duplex.push(null);
    let readSettled = false;
    const tail = reader.read().then((result) => {
      readSettled = true;
      return result;
    });
    await scheduler.wait(5);
    strictEqual(duplex.readableEnded, true);
    strictEqual(readSettled, false);
    await writable.getWriter().close();
    strictEqual((await tail).done, true);
  },
};
