// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Reproduction tests for: socket.closed rejects with undefined when socket.close()
// is called after remote FIN.
//
// When the remote peer sends FIN (closes its write side) with allowHalfOpen=false
// (the default), calling socket.close() after draining the readable to EOF causes
// socket.closed to reject with undefined instead of resolving.
//
// Two code paths race:
//   Path A (handleReadableEof): queues maybeCloseWriteSide as a microtask on EOF
//   Path B (Socket::close): calls writable abort(js, kj::none) synchronously
//
// Path B runs first (synchronous after for-await exits), putting the writable into
// Errored(undefined). When Path A's microtask fires, close(js) fails because the
// writable is errored, and the catch handler rejects closedResolver with undefined.

import { connect } from 'cloudflare:sockets';
import { strictEqual, notStrictEqual } from 'assert';

// Server: writes a small payload then closes (sends FIN).
export default {
  async connect(socket) {
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode('ping'));
    await writer.close();
  },
};

// Test 1 (core bug): socket.close() after EOF causes socket.closed to reject with undefined.
// The for-await exits on EOF, then socket.close() races with the maybeCloseWriteSide microtask.
export const closedRejectsUndefinedAfterCloseRace = {
  async test() {
    const socket = connect('localhost:8084');
    await socket.opened;

    const dec = new TextDecoder();
    let data = '';
    for await (const chunk of socket.readable) {
      data += dec.decode(chunk, { stream: true });
    }
    data += dec.decode();
    strictEqual(data, 'ping');

    // This is what application cleanup code does — close the socket after the read loop.
    // It races with maybeCloseWriteSide (triggered by EOF with allowHalfOpen=false).
    socket.close();

    // socket.closed must resolve (not reject with undefined).
    try {
      await socket.closed;
    } catch (e) {
      notStrictEqual(
        e,
        undefined,
        'socket.closed rejected with undefined — should resolve or reject with a real Error'
      );
    }
  },
};

// Test 2: if socket.closed rejects via any path, the reason must not be undefined.
export const closedRejectionReasonIsNeverUndefined = {
  async test() {
    const socket = connect('localhost:8084');
    await socket.opened;

    // Close immediately — don't drain readable.
    socket.close();

    try {
      await socket.closed;
    } catch (e) {
      notStrictEqual(e, undefined, 'socket.closed rejected with undefined');
    }
  },
};

// Test 3: without socket.close(), a clean remote FIN should resolve socket.closed.
export const closedResolvesAfterRemoteEofWithoutExplicitClose = {
  async test() {
    const socket = connect('localhost:8084');
    await socket.opened;

    const dec = new TextDecoder();
    let data = '';
    for await (const chunk of socket.readable) {
      data += dec.decode(chunk, { stream: true });
    }
    data += dec.decode();
    strictEqual(data, 'ping');

    // Do NOT call socket.close() — let maybeCloseWriteSide handle everything.
    await socket.closed;
  },
};
