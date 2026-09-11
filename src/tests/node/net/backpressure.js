// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Flow control across the socket: pause() stops the BYOB read loop and
// resume() restarts it (data is neither lost nor delivered while paused);
// write() reports backpressure against the writable highWaterMark and
// 'drain' follows once the writer has flushed; cork() batches
// synchronously.

import { strictEqual, ok, deepStrictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';
import { echo, ticker, once } from 'servers';

// While paused no 'data' fires even though the peer keeps sending; resuming
// delivers what was held and what follows.
export const pauseStopsDelivery = {
  async test(ctrl, env) {
    const socket = ticker(env);
    let ticks = '';
    socket.setEncoding('utf8');
    socket.on('data', (chunk) => (ticks += chunk));
    await once(socket, 'data');
    socket.pause();
    strictEqual(socket.isPaused(), true);
    const atPause = ticks.length;
    await scheduler.wait(100);
    strictEqual(ticks.length, atPause);
    socket.resume();
    strictEqual(socket.isPaused(), false);
    await once(socket, 'data');
    ok(ticks.length > atPause);
    ok(ticks.startsWith('tick'));
    socket.end();
    await once(socket, 'close');
  },
};

// write() returns false once the writable buffer exceeds highWaterMark,
// and 'drain' fires after the writer flushes it.
export const writeBackpressureAndDrain = {
  async test(ctrl, env) {
    const socket = echo(env, { highWaterMark: 1 });
    socket.resume();
    await once(socket, 'connect');
    let drains = 0;
    socket.on('drain', () => drains++);
    const first = socket.write(Buffer.alloc(1024, 1));
    strictEqual(first, false);
    strictEqual(socket.writableNeedDrain, true);
    await once(socket, 'drain');
    strictEqual(drains, 1);
    strictEqual(socket.writableNeedDrain, false);
    strictEqual(socket.writableLength, 0);
    socket.end();
    await once(socket, 'close');
  },
};

// cork()/uncork() cycles keep write() truthy under the default high-water
// mark and deliver every chunk.
export const corkCyclesStaySynchronous = {
  async test(ctrl, env) {
    const N = 100;
    const buf = Buffer.alloc(2, 'a');
    const socket = echo(env);
    await once(socket, 'connect');
    let accepted = true;
    let i = 0;
    for (; i < N && accepted; i++) {
      socket.cork();
      socket.write(buf);
      accepted = socket.write(buf);
      socket.uncork();
    }
    strictEqual(i, N);
    const chunks = [];
    socket.on('data', (chunk) => chunks.push(chunk));
    socket.end();
    await once(socket, 'end');
    strictEqual(Buffer.concat(chunks).byteLength, N * 4);
    deepStrictEqual([...new Set(Buffer.concat(chunks))], [0x61]);
  },
};
