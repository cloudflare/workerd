// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Data through the socket in both directions against the echo server: the
// BYOB read loop delivering Buffers (or decoded strings), the writer
// carrying strings and byte chunks, byte accounting, and the event order
// at completion.

import { strictEqual, ok, deepStrictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';
import { echo, once, readAll } from 'servers';

const PATTERN_MODULUS = 251;

function patternChunk(offset, length) {
  const chunk = Buffer.alloc(length);
  for (let i = 0; i < length; i++) chunk[i] = (offset + i) % PATTERN_MODULUS;
  return chunk;
}

// Write, half-close, read the echo to EOF: chunks are Buffers, and the
// events run 'end' → 'finish' order-agnostic but both before 'close'.
export const echoDeliversBuffers = {
  async test(ctrl, env) {
    const socket = echo(env);
    const events = [];
    socket.on('end', () => events.push('end'));
    socket.on('finish', () => events.push('finish'));
    const closed = once(socket, 'close');
    const chunks = readAll(socket);
    socket.write('hello ');
    socket.end('sockets');
    const received = await chunks;
    for (const chunk of received) ok(Buffer.isBuffer(chunk));
    strictEqual(Buffer.concat(received).toString(), 'hello sockets');
    await closed;
    ok(events.includes('end'));
    ok(events.includes('finish'));
    strictEqual(socket.destroyed, true);
  },
};

// setEncoding decodes the echoed bytes; a latin1 round trip of every byte
// value survives.
export const echoWithEncoding = {
  async test(ctrl, env) {
    const socket = echo(env);
    socket.setEncoding('latin1');
    let result = '';
    socket.on('data', (chunk) => {
      strictEqual(typeof chunk, 'string');
      result += chunk;
    });
    let expected = '';
    for (let i = 255; i >= 0; i--) {
      socket.write(String.fromCharCode(i), 'latin1');
      expected += String.fromCharCode(i);
    }
    socket.end();
    await once(socket, 'close');
    strictEqual(result, expected);
  },
};

// A 40 KiB string of multi-byte characters round-trips through utf8
// decoding across TCP fragmentation.
export const echoLargeMultiByteString = {
  async test(ctrl, env) {
    const socket = echo(env);
    const size = 40 * 1024;
    const data = 'あ'.repeat(size);
    let response = '';
    socket.setEncoding('utf8');
    socket.on('data', (chunk) => (response += chunk));
    socket.end(data);
    await once(socket, 'close');
    strictEqual(response.length, size);
    strictEqual(response, data);
  },
};

// A 10 MB write reports its full size in bytesWritten once its callback
// runs.
export const bytesWrittenLarge = {
  async test(ctrl, env) {
    const N = 10_000_000;
    const socket = echo(env);
    socket.resume();
    await new Promise((resolve) => socket.end(Buffer.alloc(N), resolve));
    strictEqual(socket.bytesWritten, N);
    await once(socket, 'close');
  },
};

// 256 KiB of a continuous pattern, produced and consumed concurrently,
// byte-exact.
export const echoLargeVolume = {
  async test(ctrl, env) {
    const TOTAL = 256 * 1024;
    const CHUNK = 16 * 1024;
    const socket = echo(env);
    const consumer = (async () => {
      let received = 0;
      for await (const chunk of socket) {
        for (let i = 0; i < chunk.byteLength; i++) {
          if (chunk[i] !== (received + i) % PATTERN_MODULUS) {
            strictEqual(
              chunk[i],
              (received + i) % PATTERN_MODULUS,
              `pattern break at byte ${received + i}`
            );
          }
        }
        received += chunk.byteLength;
      }
      return received;
    })();
    for (let offset = 0; offset < TOTAL; offset += CHUNK) {
      const flushed = new Promise((resolve) =>
        socket.write(patternChunk(offset, CHUNK), resolve)
      );
      await flushed;
    }
    socket.end();
    strictEqual(await consumer, TOTAL);
  },
};

// Chunks written with cork/uncork are batched into one writev, arrive in
// order, and count in bytesWritten immediately.
export const corkedWritesBatch = {
  async test(ctrl, env) {
    const socket = echo(env);
    socket.cork();
    socket.write('one');
    socket.write(Buffer.from('twø', 'utf8'));
    socket.uncork();
    strictEqual(socket.bytesWritten, 3 + 4);
    const chunks = readAll(socket);
    socket.end();
    deepStrictEqual(Buffer.concat(await chunks).toString(), 'onetwø');
    strictEqual(socket.bytesWritten, 3 + 4);
  },
};

// bytesRead counts echoed bytes as they are pushed; bytesWritten is the
// flushed count once the writable side has finished.
export const byteAccounting = {
  async test(ctrl, env) {
    const socket = echo(env);
    let delivered = 0;
    socket.on('data', (chunk) => (delivered += chunk.byteLength));
    const ended = once(socket, 'end');
    socket.write('hello');
    socket.end();
    await ended;
    strictEqual(delivered, 5);
    strictEqual(socket.bytesRead, 5);
    strictEqual(socket.bytesWritten, 5);
  },
};
