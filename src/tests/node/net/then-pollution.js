// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Promise.prototype.then patched by user code: the socket, which uses no
// primordials, runs its connection, reads and writes through the patched
// then; a transparent patch changes nothing about the data, and a patch
// that throws during the connection's setup surfaces as the socket's
// 'error'.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { echo, once } from 'servers';

async function withPatchedThen(patch, fn) {
  const original = Promise.prototype.then;
  Promise.prototype.then = function (onFulfilled, onRejected) {
    return patch.call(this, original, onFulfilled, onRejected);
  };
  try {
    await fn();
  } finally {
    Promise.prototype.then = original;
  }
}

// A transparent then sees the socket's hops (the count is not pinned) and
// the echo round trip is intact.
export const patchedThenPassthroughKeepsData = {
  async test(ctrl, env) {
    let calls = 0;
    let echoed;
    await withPatchedThen(
      function (original, onFulfilled, onRejected) {
        calls++;
        return original.call(this, onFulfilled, onRejected);
      },
      async () => {
        const socket = echo(env);
        await once(socket, 'connect');
        socket.write('ping');
        echoed = await new Promise((resolve) =>
          socket.once('data', (chunk) => resolve(chunk.toString()))
        );
        socket.end();
        await once(socket, 'close');
      }
    );
    strictEqual(echoed, 'ping');
    strictEqual(calls > 0, true);
  },
};

// A then that throws once, during connect(): connect() itself returns the
// socket, which then reports the throw as its 'error' and closes with
// hadError; 'connect' never fires.
export const hostileThenDuringConnectErrorsSocket = {
  async test(ctrl, env) {
    const events = [];
    const boom = new Error('hostile then');
    await withPatchedThen(
      function (original, onFulfilled, onRejected) {
        if (events.length === 0) {
          events.push('thrown');
          throw boom;
        }
        return original.call(this, onFulfilled, onRejected);
      },
      async () => {
        const socket = echo(env);
        socket.on('connect', () => events.push('connect'));
        socket.on('error', (err) => events.push(['error', err]));
        socket.on('close', (hadError) => events.push(['close', hadError]));
        await once(socket, 'close');
        strictEqual(socket.destroyed, true);
      }
    );
    deepStrictEqual(events, ['thrown', ['error', boom], ['close', true]]);
  },
};
