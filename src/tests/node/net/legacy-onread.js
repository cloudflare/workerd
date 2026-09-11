// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without streams_byob_reader_detaches_buffer the C++ implementation fills a
// BYOB read's view in place: the onread fixed buffer is never transferred,
// the caller's Uint8Array keeps its extent, and every callback sees a
// Buffer over that very same backing store.

import net from 'node:net';
import { strictEqual, ok } from 'node:assert';
import { Buffer } from 'node:buffer';

function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

// As servers.js's echoSegments (this cell embeds only this module): each
// segment's echo is awaited before the next is written.
async function echoSegments(socket, segments, receivedLength) {
  let sent = 0;
  for (const segment of segments) {
    socket.write(segment);
    sent += Buffer.byteLength(segment);
    for (let i = 0; receivedLength() < sent; i++) {
      ok(i < 2000, `echo of ${JSON.stringify(segment)} never arrived`);
      await scheduler.wait(2);
    }
  }
}

export const legacyFixedBufferIsFilledInPlace = {
  async test(ctrl, env) {
    const buffer = Buffer.alloc(1024);
    let received = '';
    let lastFill = '';
    // Observed in the callback, asserted afterwards: an assertion thrown
    // from inside the callback would only stop the read loop.
    const fills = [];
    const socket = net.connect({
      host: env.SIDECAR_HOSTNAME,
      port: Number(env.NET_ECHO_PORT),
      onread: {
        buffer,
        callback(nread, buf) {
          fills.push({
            nread,
            sameBacking: buf.buffer === buffer.buffer,
            callerLength: buffer.byteLength,
          });
          lastFill = buf.toString();
          received += lastFill;
        },
      },
    });
    await once(socket, 'connect');
    await echoSegments(
      socket,
      ['first', 'second', 'third'],
      () => received.length
    );
    strictEqual(received, 'firstsecondthird');
    ok(fills.length >= 3, `expected three fills or more, got ${fills.length}`);
    for (const fill of fills) {
      strictEqual(fill.sameBacking, true);
      strictEqual(fill.callerLength, 1024);
    }
    strictEqual(buffer.byteLength, 1024);
    // The last fill is still readable from the caller's buffer.
    strictEqual(buffer.toString('utf8', 0, fills.at(-1).nread), lastFill);
    const closed = once(socket, 'close');
    socket.end();
    await closed;
  },
};

// A fixed buffer that is a view into a larger allocation is filled in place
// within its own range only: the bytes around it survive every fill.
export const legacySubarrayIsFilledWithinItsRange = {
  async test(ctrl, env) {
    const backing = new ArrayBuffer(256);
    const all = new Uint8Array(backing).fill(0xaa);
    const view = new Uint8Array(backing, 64, 32);
    let received = '';
    let lastFill = '';
    // Observed in the callback, asserted afterwards: an assertion thrown
    // from inside the callback would only stop the read loop.
    const fills = [];
    const socket = net.connect({
      host: env.SIDECAR_HOSTNAME,
      port: Number(env.NET_ECHO_PORT),
      onread: {
        buffer: view,
        callback(nread, buf) {
          fills.push({
            nread,
            byteOffset: buf.byteOffset,
            sameBacking: buf.buffer === backing,
          });
          lastFill = buf.toString();
          received += lastFill;
        },
      },
    });
    await once(socket, 'connect');
    await echoSegments(
      socket,
      ['first', 'second', 'third'],
      () => received.length
    );
    strictEqual(received, 'firstsecondthird');
    ok(fills.length >= 3, `expected three fills or more, got ${fills.length}`);
    for (const fill of fills) {
      ok(fill.nread <= 32, `fill of ${fill.nread} exceeds the view's 32 bytes`);
      strictEqual(fill.byteOffset, 64);
      strictEqual(fill.sameBacking, true);
    }
    strictEqual(view.byteLength, 32);
    ok(all.subarray(0, 64).every((b) => b === 0xaa));
    ok(all.subarray(96).every((b) => b === 0xaa));
    // The last fill is still readable from the caller's view.
    strictEqual(
      Buffer.from(backing, 64, fills.at(-1).nread).toString(),
      lastFill
    );
    const closed = once(socket, 'close');
    socket.end();
    await closed;
  },
};
