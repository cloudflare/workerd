// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Re-entering the socket from its own callbacks: writing, ending and
// destroying from inside the onread callback or a 'data' listener, and
// toggling pause()/resume() there. One read loop runs whatever happens
// inside a delivery, and the writes such re-entries queue are flushed.

import { strictEqual, deepStrictEqual, ok } from 'node:assert';
import { Buffer } from 'node:buffer';
import { echo, sink, ticker, once } from 'servers';

// Writing from inside the onread callback: each echoed fill triggers the
// next write, in order, until the callback ends the socket.
export const writeFromOnreadCallback = {
  async test(ctrl, env) {
    const fills = [];
    const socket = echo(env, {
      onread: {
        buffer: Buffer.alloc(64),
        callback(nread, buf) {
          fills.push(buf.toString('latin1', 0, nread));
          if (fills.length < 3) socket.write(`round-${fills.length}`);
          else socket.end();
        },
      },
    });
    await once(socket, 'connect');
    const closed = once(socket, 'close');
    socket.write('round-0');
    await closed;
    deepStrictEqual(fills, ['round-0', 'round-1', 'round-2']);
  },
};

// destroy() from inside the onread callback: the fill being delivered is
// the last, the loop stops, 'close' follows without an error.
export const destroyFromOnreadCallback = {
  async test(ctrl, env) {
    const fills = [];
    const socket = echo(env, {
      onread: {
        buffer: Buffer.alloc(64),
        callback(nread) {
          fills.push(nread);
          socket.destroy();
        },
      },
    });
    socket.on('error', (err) => {
      throw err;
    });
    await once(socket, 'connect');
    const closed = once(socket, 'close');
    socket.write('xyz');
    const hadError = await closed;
    strictEqual(hadError, false);
    deepStrictEqual(fills, [3]);
    strictEqual(socket.destroyed, true);
  },
};

// pause() and resume() toggled several times synchronously inside 'data':
// one read loop keeps running (resume() never starts a second one), and no
// tick is lost or duplicated.
export const pauseResumeStormInsideData = {
  async test(ctrl, env) {
    const socket = ticker(env);
    let text = '';
    socket.on('data', (chunk) => {
      text += chunk.toString();
      socket.pause();
      socket.resume();
      socket.pause();
      socket.resume();
    });
    await once(socket, 'connect');
    while (text.length < 'tick'.repeat(4).length) await scheduler.wait(5);
    strictEqual(socket._handle.readLoopActive, true);
    socket.end();
    await once(socket, 'close');
    ok(/^(tick)+$/.test(text), text);
  },
};

// end() issued from inside 'data' right after queueing a large write and a
// small one: every queued byte is flushed before the FIN, and the echo of
// all of it arrives before 'end'.
export const endInsideDataFlushesQueuedWrites = {
  async test(ctrl, env) {
    const socket = echo(env);
    await once(socket, 'connect');
    let received = 0;
    let ended = false;
    socket.on('data', (chunk) => {
      received += chunk.length;
      if (!ended) {
        ended = true;
        socket.write(Buffer.alloc(100_000, 1));
        socket.write('z');
        socket.end();
      }
    });
    const end = once(socket, 'end');
    const closed = once(socket, 'close');
    socket.write('abc');
    await end;
    strictEqual(received, 3 + 100_000 + 1);
    await closed;
  },
};

// end() issued from a write callback while earlier writes are still
// queued: the sink counts every byte of every write.
export const endFromWriteCallbackFlushesQueue = {
  async test(ctrl, env) {
    const socket = sink(env);
    await once(socket, 'connect');
    const chunk = Buffer.alloc(200_000, 7);
    let callbacks = 0;
    for (let i = 0; i < 5; i++) socket.write(chunk, () => callbacks++);
    socket.write('tail', () => {
      callbacks++;
      socket.end();
    });
    const reply = await new Promise((resolve) =>
      socket.once('data', (data) => resolve(data.toString()))
    );
    await once(socket, 'close');
    strictEqual(Number(reply), 5 * 200_000 + 4);
    strictEqual(callbacks, 6);
  },
};
