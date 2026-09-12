// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Ending and destroying: end() closes the writer and finishes; destroy()
// closes the connect() socket and emits 'close' (after 'error' when given
// one); writes after destroy fail with ERR_STREAM_DESTROYED; a socket
// whose handle is gone fails writes with ERR_SOCKET_CLOSED; property access
// on a closed socket is harmless.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';
import { echo, endsImmediately, sink, resetter, once } from 'servers';

// bufferSize reflects the queued-but-unflushed bytes while writing and is 0
// once finished.
export const bufferSizeTracksQueue = {
  async test(ctrl, env) {
    const socket = endsImmediately(env);
    socket.resume();
    strictEqual(socket.bufferSize, 0);
    socket.write('a');
    socket.end();
    strictEqual(socket.bufferSize, 1);
    const closed = once(socket, 'close');
    await once(socket, 'finish');
    strictEqual(socket.bufferSize, 0);
    await closed;
  },
};

// destroy() without an error: 'close' with hadError false, no 'error'.
export const destroyWithoutError = {
  async test(ctrl, env) {
    const socket = echo(env);
    await once(socket, 'connect');
    const errors = [];
    socket.on('error', (err) => errors.push(err));
    const closed = new Promise((resolve) => socket.once('close', resolve));
    strictEqual(socket.destroy(), socket);
    strictEqual(await closed, false);
    deepStrictEqual(errors, []);
    strictEqual(socket.destroyed, true);
    strictEqual(socket.readyState, 'closed');
  },
};

// A write after destroy() fails with ERR_STREAM_DESTROYED; repeated
// destroy() calls return the socket.
export const writeAfterDestroy = {
  async test(ctrl, env) {
    const socket = endsImmediately(env);
    await once(socket, 'connect');
    strictEqual(socket, socket.destroy().destroy());
    socket.on('error', () => {
      throw new Error('no error event expected');
    });
    const err = await new Promise((resolve) =>
      socket.write(Buffer.from('kaboom'), resolve)
    );
    strictEqual(err.code, 'ERR_STREAM_DESTROYED');
    strictEqual(err.name, 'Error');
    strictEqual(err.message, 'Cannot call write after a stream was destroyed');
  },
};

// A write on a socket that has lost its handle fails with ERR_SOCKET_CLOSED.
export const writeWithoutHandle = {
  async test(ctrl, env) {
    const socket = endsImmediately(env);
    await once(socket, 'connect');
    const errored = once(socket, 'error');
    socket._handle = null;
    socket.write('foo');
    const err = await errored;
    strictEqual(err.name, 'Error');
    strictEqual(err.message, 'Socket is closed');
    strictEqual(err.code, 'ERR_SOCKET_CLOSED');
  },
};

// Writes of non-byte chunks throw ERR_INVALID_ARG_TYPE synchronously.
export const writeRejectsInvalidChunk = {
  async test(ctrl, env) {
    const socket = endsImmediately(env);
    socket.resume();
    await once(socket, 'connect');
    socket.on('error', () => {
      throw new Error('no error event expected');
    });
    let thrown;
    try {
      socket.write(1337);
    } catch (err) {
      thrown = err;
    }
    strictEqual(thrown?.code, 'ERR_INVALID_ARG_TYPE');
    strictEqual(thrown?.name, 'TypeError');
    socket.destroy();
    await once(socket, 'close');
  },
};

// Everything written before end() reaches the peer: the sink server reports
// the byte count it received.
export const endFlushesQueuedWrites = {
  async test(ctrl, env) {
    const socket = sink(env);
    let report = '';
    socket.setEncoding('utf8');
    socket.on('data', (chunk) => (report += chunk));
    for (let i = 0; i < 100; i++) socket.write(Buffer.alloc(1000, i));
    socket.end(Buffer.alloc(7));
    await once(socket, 'end');
    strictEqual(report, String(100 * 1000 + 7));
  },
};

// end() with and without data/encoding invokes its callback once the
// writable side has finished.
export const endCallbackForms = {
  async test(ctrl, env) {
    for (const args of [[], ['foo'], ['foo', 'utf8']]) {
      const socket = endsImmediately(env);
      socket.resume();
      await once(socket, 'connect');
      await new Promise((resolve) => socket.end(...args, resolve));
      strictEqual(socket.writableFinished, true);
      await once(socket, 'close');
    }
  },
};

// Property access and no-op methods on a closed socket do not throw.
export const closedSocketIsInert = {
  async test(ctrl, env) {
    const socket = endsImmediately(env);
    socket.resume();
    await once(socket, 'close');
    socket.setNoDelay();
    socket.setKeepAlive();
    socket.pause();
    socket.resume();
    socket.address();
    for (const property of [
      'bufferSize',
      'remoteAddress',
      'remotePort',
      'remoteFamily',
      'bytesRead',
      'bytesWritten',
    ]) {
      void socket[property];
    }
    const during = endsImmediately(env);
    during.destroy();
    void during.remoteAddress;
    void during.remoteFamily;
    void during.remotePort;
    await once(during, 'close');
  },
};

// destroy(err): 'error' with that error, then 'close' with hadError true.
export const destroyWithError = {
  async test(ctrl, env) {
    const socket = echo(env);
    await once(socket, 'connect');
    const events = [];
    socket.on('error', (err) => events.push(['error', err]));
    const closed = new Promise((resolve) =>
      socket.once('close', (hadError) => {
        events.push(['close', hadError]);
        resolve();
      })
    );
    const boom = new Error('torn down');
    socket.destroy(boom);
    await closed;
    deepStrictEqual(events, [
      ['error', boom],
      ['close', true],
    ]);
  },
};

// The peer resets the connection (RST, no FIN) while the socket is
// reading: the connection's failure surfaces as 'error' (a plain Error
// without a code, where Node reports ECONNRESET), then 'close' with
// hadError true, and the socket is destroyed with that error. The runtime
// may report the readable's end before the failure, in which case 'end'
// precedes the error (Node emits no 'end' on a reset); the order of the
// two is not pinned.
export const peerResetMidReadErrors = {
  async test(ctrl, env) {
    const socket = resetter(env);
    const events = [];
    socket.on('data', (chunk) => events.push(`data(${chunk})`));
    let ended = false;
    socket.on('end', () => (ended = true));
    socket.on('error', (err) => {
      events.push('error');
      strictEqual(err instanceof Error, true);
      strictEqual(err.code, undefined);
      strictEqual(socket.errored, err);
    });
    const closed = new Promise((resolve) =>
      socket.once('close', (hadError) => {
        events.push(`close(${hadError})`);
        resolve();
      })
    );
    await once(socket, 'data');
    socket.write('go');
    await closed;
    deepStrictEqual(events, ['data(ready)', 'error', 'close(true)']);
    strictEqual(socket.destroyed, true);
    strictEqual(typeof ended, 'boolean');
  },
};

// Writes after the reset fail through their callbacks with
// ERR_STREAM_DESTROYED (the socket was torn down by the failure), return
// false, and add no 'error' of their own.
export const peerResetMidWriteFailsCallbacks = {
  async test(ctrl, env) {
    const socket = resetter(env);
    const errors = [];
    socket.on('data', () => {});
    socket.on('error', (err) => errors.push(err));
    await once(socket, 'data');
    socket.write('go');
    await once(socket, 'close');
    const callbacks = [];
    const returned = [];
    for (let i = 0; i < 3; i++) {
      returned.push(
        socket.write(Buffer.alloc(64 * 1024, 1), (err) =>
          callbacks.push(err?.code)
        )
      );
    }
    await scheduler.wait(20);
    deepStrictEqual(returned, [false, false, false]);
    deepStrictEqual(callbacks, [
      'ERR_STREAM_DESTROYED',
      'ERR_STREAM_DESTROYED',
      'ERR_STREAM_DESTROYED',
    ]);
    strictEqual(errors.length, 1);
    strictEqual(socket.writable, false);
  },
};
