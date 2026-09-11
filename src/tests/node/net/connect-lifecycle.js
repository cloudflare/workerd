// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// How a node:net Socket comes up over a connect() socket: the handle it
// keeps (a BYOB reader over the readable half, a default writer over the
// writable half, both locked for the socket's lifetime), the connect/ready
// sequence, the state properties across it, and what happens to writes
// issued before the connection is up.

import net from 'node:net';
import { strictEqual, ok, deepStrictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';
import { echo, endsImmediately, once } from 'servers';

// The socket's handle wraps the connect() socket's halves with a BYOB
// reader and a default writer, taking both locks.
export const handleLocksBothHalves = {
  async test(ctrl, env) {
    const socket = echo(env);
    socket.resume();
    ok(socket._handle.reader instanceof ReadableStreamBYOBReader);
    ok(socket._handle.writer instanceof WritableStreamDefaultWriter);
    ok(socket._handle.socket.readable instanceof ReadableStream);
    ok(socket._handle.socket.writable instanceof WritableStream);
    strictEqual(socket._handle.socket.readable.locked, true);
    strictEqual(socket._handle.socket.writable.locked, true);
    await once(socket, 'connect');
    socket.destroy();
    await once(socket, 'close');
  },
};

// 'connect' then 'ready' fire once the connect() socket opens; state
// properties track the transition.
export const connectThenReady = {
  async test(ctrl, env) {
    const socket = echo(env);
    strictEqual(socket.pending, true);
    strictEqual(socket.connecting, true);
    strictEqual(socket.readyState, 'opening');
    const events = [];
    socket.on('connect', () => events.push('connect'));
    socket.on('ready', () => events.push('ready'));
    await once(socket, 'ready');
    deepStrictEqual(events, ['connect', 'ready']);
    strictEqual(socket.pending, false);
    strictEqual(socket.connecting, false);
    strictEqual(socket.readyState, 'open');
    socket.resume();
    socket.end();
    await once(socket, 'close');
    strictEqual(socket.readyState, 'closed');
  },
};

// A write issued before 'connect' and one issued in the 'connect' handler
// arrive in order.
export const writeBeforeAndAfterConnect = {
  async test(ctrl, env) {
    const socket = echo(env);
    let received = '';
    socket.setEncoding('utf8');
    socket.on('connect', () => socket.write(' after'));
    socket.on('data', (chunk) => {
      received += chunk;
      if (received === 'before after') socket.end();
    });
    socket.write('before');
    await once(socket, 'end');
    strictEqual(received, 'before after');
  },
};

// Destroying before the connection opens: 'connect' never fires, and a
// pending write fails with ERR_SOCKET_CLOSED_BEFORE_CONNECTION.
export const destroyBeforeConnect = {
  async test(ctrl, env) {
    const socket = new net.Socket();
    socket.on('connect', () => {
      throw new Error('connect must not fire');
    });
    socket.connect(Number(env.NET_END_PORT), env.SIDECAR_HOSTNAME);
    ok(socket.connecting);
    const written = new Promise((resolve) => socket.write('foo', resolve));
    const closed = once(socket, 'close');
    socket.destroy();
    const err = await written;
    strictEqual(err.code, 'ERR_SOCKET_CLOSED_BEFORE_CONNECTION');
    strictEqual(err.name, 'Error');
    await closed;
  },
};

// An immediate destroy() after connect() never emits 'connect'.
export const immediateDestroySkipsConnect = {
  async test(ctrl, env) {
    let connects = 0;
    const socket = endsImmediately(env);
    socket.on('connect', () => connects++);
    const closed = once(socket, 'close');
    socket.destroy();
    await closed;
    strictEqual(connects, 0);
  },
};

// Writes issued while connecting are held and flushed after 'connect'; the
// write callback observes the connected state. bytesWritten counts bytes,
// not characters.
export const writesBeforeConnectAreDeferred = {
  async test(ctrl, env) {
    const socket = echo(env, { highWaterMark: 0 });
    strictEqual(socket.bytesWritten, 0);
    const a = "L'État, c'est ";
    const b = 'moi';
    let result = '';
    socket.setEncoding('utf8');
    socket.on('data', (chunk) => (result += chunk));
    const written = new Promise((resolve) => socket.write(a, resolve));
    const closed = once(socket, 'close');
    socket.end(b);
    await written;
    strictEqual(socket.pending, false);
    strictEqual(socket.connecting, false);
    strictEqual(socket.readyState, 'readOnly');
    strictEqual(socket.bytesWritten, Buffer.byteLength(a + b));
    await closed;
    strictEqual(result, a + b);
  },
};
