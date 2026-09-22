// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A node:net Socket meeting web streams through the node:stream adapters
// and pipeline: it is a Duplex over a connect() socket whose halves are
// themselves web streams, so these paths wrap web streams around a node
// stream around web streams.

import { Readable, Writable, Duplex, pipeline } from 'node:stream';
import { strictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';
import { echo, greet, once, GREETING } from 'servers';

const enc = new TextEncoder();
const dec = new TextDecoder();

// socket.pipe() into a Writable.fromWeb sink delivers the echo and closes
// the web sink.
export const pipeIntoWritableFromWeb = {
  async test(ctrl, env) {
    const seen = [];
    let closed = false;
    const sink = Writable.fromWeb(
      new WritableStream({
        write(chunk) {
          seen.push(dec.decode(chunk));
        },
        close() {
          closed = true;
        },
      })
    );
    const socket = echo(env);
    socket.pipe(sink);
    socket.end('piped through the socket');
    await once(sink, 'finish');
    strictEqual(seen.join(''), 'piped through the socket');
    strictEqual(closed, true);
  },
};

// Readable.toWeb(socket) serves as a Response body. The adapter observes
// the whole Duplex, so the body reaches EOF only once the socket's writable
// side has finished too: end() first, then read the greeting to completion.
export const socketAsResponseBody = {
  async test(ctrl, env) {
    const socket = greet(env);
    socket.end();
    const text = await new Response(Readable.toWeb(socket)).text();
    strictEqual(text, GREETING);
  },
};

// pipeline(socket, TransformStream, node sink): the socket as a web-fed
// pipeline source.
export const pipelineSocketThroughWebTransform = {
  async test(ctrl, env) {
    const socket = greet(env);
    socket.end();
    const upper = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(enc.encode(dec.decode(chunk).toUpperCase()));
      },
    });
    const chunks = [];
    const sink = new Writable({
      write(chunk, encoding, callback) {
        chunks.push(chunk);
        callback();
      },
    });
    await new Promise((resolve) => pipeline(socket, upper, sink, resolve));
    strictEqual(Buffer.concat(chunks).toString(), GREETING.toUpperCase());
  },
};

// pipeline(web ReadableStream, socket): a web source pumped into the
// socket; the echo comes back through the socket's readable side.
export const pipelineWebSourceIntoSocket = {
  async test(ctrl, env) {
    const socket = echo(env);
    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('from '));
        controller.enqueue(enc.encode('the web'));
        controller.close();
      },
    });
    const chunks = [];
    socket.on('data', (chunk) => chunks.push(chunk));
    const ended = once(socket, 'end');
    await new Promise((resolve) => pipeline(source, socket, resolve));
    await ended;
    strictEqual(Buffer.concat(chunks).toString(), 'from the web');
  },
};

// Duplex.toWeb(socket): a web pair over the socket; writes through the
// writer are echoed to the reader, and closing the writer half-closes the
// socket so the reader reaches EOF.
export const duplexToWebRoundTrip = {
  async test(ctrl, env) {
    const socket = echo(env);
    const { readable, writable } = Duplex.toWeb(socket);
    const writer = writable.getWriter();
    await writer.write(enc.encode('round '));
    await writer.write(enc.encode('trip'));
    const closing = writer.close();
    const reader = readable.getReader();
    let received = '';
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      received += dec.decode(value);
    }
    strictEqual(received, 'round trip');
    await closing;
  },
};

// Readable.fromWeb over the connect() socket's own readable half would
// need the lock the node socket already holds: a second consumer is
// refused.
export const socketHalvesAreLocked = {
  async test(ctrl, env) {
    const socket = echo(env);
    let thrown;
    try {
      Readable.fromWeb(socket._handle.socket.readable);
    } catch (err) {
      thrown = err;
    }
    strictEqual(thrown?.name, 'TypeError');
    try {
      Writable.fromWeb(socket._handle.socket.writable);
    } catch (err) {
      thrown = err;
    }
    strictEqual(thrown?.name, 'TypeError');
    socket.destroy();
    await once(socket, 'close');
  },
};
