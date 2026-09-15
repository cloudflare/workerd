// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// GC interactions of the toWeb adapters. A web stream produced by toWeb is
// often reachable only through the node stream it adapts (whose pending
// I/O keeps it alive) and through whatever awaits its reader or writer;
// the node stream keeps the web stream alive, so a forced GC in that
// window collects nothing that a later push, error or close needs to reach.
// Requires --expose-gc (set in the three cell configs).

import { Readable, Writable, Duplex } from 'node:stream';
import { strictEqual, rejects } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

// Several forced GCs, a few milliseconds apart: references held from C++
// start out strong and switch to traced mode only once their owner has
// been traced by a GC, so the first GC only arms the collection.
function collectSoon() {
  for (const at of [5, 9, 13]) setTimeout(() => gc(), at);
}

// A read awaited by nothing but its own continuation, with the source's
// data arriving from a timer after a forced GC: the read resolves.
export const toWebPendingReadSurvivesGc = {
  async test() {
    const r = new Readable({ read() {} });
    const reader = Readable.toWeb(r).getReader();
    collectSoon();
    setTimeout(() => {
      r.push(enc.encode('after gc'));
      r.push(null);
    }, 20);
    const { value } = await reader.read();
    strictEqual(dec.decode(value), 'after gc');
    strictEqual((await reader.read()).done, true);
  },
};

// A toWeb stream nobody holds — no reader, no reference — still exerts its
// backpressure on the source after a forced GC: the stream's pull() has
// resumed the source, and the one chunk its queue can take (a count
// strategy with a high-water mark of 1) pauses it again. A stream that had
// been collected would take nothing and pause nothing, leaving the source
// flowing into the void.
export const toWebUnheldStreamStillPausesSource = {
  async test() {
    const r = new Readable({ objectMode: true, highWaterMark: 1, read() {} });
    Readable.toWeb(r);
    collectSoon();
    await scheduler.wait(20);
    strictEqual(r.isPaused(), false);
    r.push('a');
    strictEqual(r.isPaused(), true);
  },
};

// Readable.toWeb() twice on one source: both streams get every chunk, and
// both stay alive through the source. The first, unheld, has a high-water
// mark of 1, the second, held, one of 4. After a forced GC, the chunk both
// receive fills the first stream's queue, which pauses the source — the
// second stream's pull() resumes it again at once, so the 'pause' event
// the first one emits is what shows it is still there.
export const toWebTwiceKeepsBothStreamsAlive = {
  async test() {
    const r = new Readable({ objectMode: true, highWaterMark: 1, read() {} });
    Readable.toWeb(r);
    const second = Readable.toWeb(r, {
      strategy: new CountQueuingStrategy({ highWaterMark: 4 }),
    });
    let pauses = 0;
    r.on('pause', () => pauses++);
    collectSoon();
    await scheduler.wait(20);
    strictEqual(r.isPaused(), false);
    r.push('a');
    strictEqual(pauses, 1);
    strictEqual((await second.getReader().read()).value, 'a');
  },
};

// writer.closed awaited by nothing but its own continuation, with the node
// sink failing from a timer after a forced GC: the rejection arrives.
export const toWebWriterClosedSurvivesGc = {
  async test() {
    const boom = new Error('late sink failure');
    const writable = new Writable({
      write(chunk, encoding, callback) {
        setTimeout(() => callback(boom), 20);
      },
    });
    writable.on('error', () => {});
    const writer = Writable.toWeb(writable).getWriter();
    await writer.write(enc.encode('a'));
    collectSoon();
    await rejects(writer.closed, (err) => err === boom);
    strictEqual(writable.destroyed, true);
  },
};

// Writable.toWeb() twice on one writable: the first stream's writer.closed,
// awaited by nothing but its own continuation, still hears the sink fail
// after a forced GC once a second stream has been adapted over the same
// writable — and so does the second.
export const toWebTwiceKeepsBothWritableStreamsAlive = {
  async test() {
    const boom = new Error('late sink failure');
    const writable = new Writable({
      write(chunk, encoding, callback) {
        setTimeout(() => callback(boom), 20);
      },
    });
    writable.on('error', () => {});
    const writer = Writable.toWeb(writable).getWriter();
    const second = Writable.toWeb(writable);
    await writer.write(enc.encode('a'));
    collectSoon();
    await rejects(writer.closed, (err) => err === boom);
    await rejects(second.getWriter().closed, (err) => err === boom);
  },
};

// The same through Duplex.toWeb, whose halves are the two adapters over
// one node stream: a pending read on the readable half and a pending
// close on the writable half both survive.
export const duplexToWebPendingOperationsSurviveGc = {
  async test() {
    const d = new Duplex({
      read() {},
      write(chunk, encoding, callback) {
        callback();
      },
      final(callback) {
        setTimeout(callback, 20);
      },
    });
    const { readable, writable } = Duplex.toWeb(d);
    const reader = readable.getReader();
    const writer = writable.getWriter();
    collectSoon();
    setTimeout(() => {
      d.push(enc.encode('x'));
      d.push(null);
    }, 25);
    const closing = writer.close();
    const { value } = await reader.read();
    strictEqual(dec.decode(value), 'x');
    strictEqual((await reader.read()).done, true);
    await closing;
  },
};
