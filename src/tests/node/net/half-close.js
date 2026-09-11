// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Peer EOF and allowHalfOpen. The read loop's done result pushes EOF; with
// allowHalfOpen false (the default) the socket then ends its writable side
// and refuses further writes with EPIPE; with allowHalfOpen true the
// writable side stays usable after 'end'.

import net from 'node:net';
import { strictEqual, ok, deepStrictEqual } from 'node:assert';
import { endsImmediately, greet, once, GREETING } from 'servers';

// The socket registers its half-open enforcer on 'end' unconditionally (it
// consults allowHalfOpen when it runs, so a later connect() can flip it);
// allowHalfOpen defaults to false.
export const defaultDisallowsHalfOpen = {
  test() {
    const socket = new net.Socket();
    strictEqual(socket.allowHalfOpen, false);
    strictEqual(socket.listenerCount('end'), 1);
    const halfOpen = new net.Socket({ allowHalfOpen: true });
    strictEqual(halfOpen.allowHalfOpen, true);
    strictEqual(halfOpen.listenerCount('end'), 1);
  },
};

// allowHalfOpen true: after the peer's EOF the writable side is still open
// and a write completes cleanly; the socket closes once the client ends its
// own side.
export const halfOpenWriteAfterPeerEof = {
  async test(ctrl, env) {
    const socket = greet(env, { allowHalfOpen: true });
    let greeting = '';
    socket.setEncoding('utf8');
    socket.on('data', (chunk) => (greeting += chunk));
    await once(socket, 'end');
    strictEqual(greeting, GREETING);
    strictEqual(socket.readable, false);
    strictEqual(socket.writable, true);
    strictEqual(socket.readyState, 'writeOnly');
    const err = await new Promise((resolve) => socket.write('bye', resolve));
    strictEqual(err, undefined);
    strictEqual(socket.destroyed, false);
    const closed = once(socket, 'close');
    socket.end();
    await closed;
    strictEqual(socket.writableFinished, true);
  },
};

// allowHalfOpen true against a server that ends at once: the socket does
// not end itself; end() from the client closes it.
export const halfOpenRequiresExplicitEnd = {
  async test(ctrl, env) {
    const socket = endsImmediately(env, { allowHalfOpen: true });
    socket.resume();
    await once(socket, 'end');
    await scheduler.wait(20);
    strictEqual(socket.writableEnded, false);
    strictEqual(socket.destroyed, false);
    ok(socket.writable);
    const closed = once(socket, 'close');
    socket.end();
    await closed;
  },
};

// allowHalfOpen false: peer EOF → 'end' (socket not yet destroyed) →
// automatic end of the writable side → 'finish' → 'close'.
export const peerEofEndsBothSides = {
  async test(ctrl, env) {
    const socket = endsImmediately(env);
    strictEqual(socket.allowHalfOpen, false);
    socket.resume();
    const events = [];
    socket.on('end', () => {
      events.push('end');
      strictEqual(socket.destroyed, false);
      strictEqual(socket.writable, true);
    });
    socket.on('finish', () => {
      events.push('finish');
      strictEqual(socket.destroyed, false);
    });
    await once(socket, 'close');
    strictEqual(events.join(','), 'end,finish');
    strictEqual(socket.destroyed, true);
  },
};

// allowHalfOpen false: a write after the peer's EOF (once the writable side
// has been ended) fails with EPIPE. By then the socket has auto-destroyed
// (both sides done), so the EPIPE destroy is a no-op and no 'error' event
// follows.
export const writeAfterPeerEofIsEpipe = {
  async test(ctrl, env) {
    const socket = endsImmediately(env);
    socket.resume();
    const errors = [];
    socket.on('error', (err) => errors.push(err));
    const closed = once(socket, 'close');
    await once(socket, 'finish');
    await closed;
    strictEqual(socket.destroyed, true);
    const err = await new Promise((resolve) => socket.write('late', resolve));
    strictEqual(err.code, 'EPIPE');
    await scheduler.wait(5);
    strictEqual(errors.length, 0);
  },
};

// The peer's EOF surfaces even when nobody is reading the socket: the read
// loop pushes EOF and reads zero bytes so an empty stream ends at once
// (Node's onStreamRead does the same), and with allowHalfOpen false the
// socket then ends its own side and closes.
export const peerEofWithoutConsumerEnds = {
  async test(ctrl, env) {
    const socket = endsImmediately(env);
    const events = [];
    socket.on('end', () => events.push('end'));
    socket.on('finish', () => events.push('finish'));
    const closed = once(socket, 'close');
    await closed;
    deepStrictEqual(events, ['end', 'finish']);
    strictEqual(socket.readableEnded, true);
    strictEqual(socket.destroyed, true);
  },
};
