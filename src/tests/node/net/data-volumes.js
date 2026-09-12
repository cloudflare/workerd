// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Data shapes the echo tests do not produce: a peer trickling one byte at
// a time (many small reads, coalescing allowed, nothing lost or
// reordered), multi-byte UTF-8 split at every byte boundary by the peer
// (setEncoding reassembles it exactly), and a burst of many one-byte
// writes (every byte counted and delivered).

import { strictEqual, ok } from 'node:assert';
import { Buffer } from 'node:buffer';
import { echo, trickle, utf8Split, UTF8_SPLIT_TEXT, sink, once } from 'servers';

const PATTERN_MODULUS = 251;

function patternChunk(offset, length) {
  const chunk = Buffer.alloc(length);
  for (let i = 0; i < length; i++) chunk[i] = (offset + i) % PATTERN_MODULUS;
  return chunk;
}

// 2000 bytes, one per millisecond: every byte arrives, in order, over many
// reads (each smaller than the whole).
export const trickledBytesArriveInOrder = {
  async test(ctrl, env) {
    const socket = trickle(env);
    const chunks = [];
    socket.on('data', (chunk) => chunks.push(chunk));
    await once(socket, 'end');
    const received = Buffer.concat(chunks);
    strictEqual(received.length, 2000);
    for (let i = 0; i < received.length; i++) {
      if (received[i] !== i % 251) {
        throw new Error(`byte ${i} is ${received[i]}, expected ${i % 251}`);
      }
    }
    ok(chunks.length > 10, `only ${chunks.length} reads`);
    strictEqual(socket.bytesRead, 2000);
    socket.end();
    await once(socket, 'close');
  },
};

// Two-, three- and four-byte sequences delivered one byte per read:
// setEncoding('utf8') reassembles the text exactly, never emitting a
// replacement character for a split sequence.
export const splitUtf8IsReassembledBySetEncoding = {
  async test(ctrl, env) {
    const socket = utf8Split(env);
    socket.setEncoding('utf8');
    let text = '';
    let events = 0;
    socket.on('data', (chunk) => {
      strictEqual(typeof chunk, 'string');
      ok(!chunk.includes('\ufffd'), `replacement character in ${chunk}`);
      text += chunk;
      events++;
    });
    await once(socket, 'end');
    strictEqual(text, UTF8_SPLIT_TEXT);
    ok(events > 1, 'the text arrived in a single event');
    socket.end();
    await once(socket, 'close');
  },
};

// Without setEncoding the same bytes arrive as Buffers whose
// concatenation decodes to the text (a decoder over the concatenation, not
// per chunk).
export const splitUtf8BytesConcatenate = {
  async test(ctrl, env) {
    const socket = utf8Split(env);
    const chunks = [];
    socket.on('data', (chunk) => chunks.push(chunk));
    await once(socket, 'end');
    strictEqual(Buffer.concat(chunks).toString('utf8'), UTF8_SPLIT_TEXT);
    strictEqual(
      Buffer.concat(chunks).length,
      Buffer.byteLength(UTF8_SPLIT_TEXT)
    );
    socket.end();
    await once(socket, 'close');
  },
};

// 20,000 one-byte writes in a burst (uncorked): bytesWritten is exact and
// the sink counts every byte.
export const manyTinyWritesAreAllDelivered = {
  async test(ctrl, env) {
    const socket = sink(env);
    await once(socket, 'connect');
    const count = 20000;
    let callbacks = 0;
    for (let i = 0; i < count; i++) {
      socket.write(Buffer.from([i % 251]), () => callbacks++);
    }
    socket.end();
    const reply = [];
    socket.on('data', (chunk) => reply.push(chunk));
    await once(socket, 'end');
    strictEqual(Buffer.concat(reply).toString(), String(count));
    strictEqual(socket.bytesWritten, count);
    strictEqual(callbacks, count);
    await once(socket, 'close');
  },
};

// A 4 MiB echo round trip with the socket paused for a moment after every
// 256 KiB received: every byte comes back, in order, and the pauses are
// honored (delivery resumes without loss).
export const largeEchoWithPauses = {
  async test(ctrl, env) {
    const TOTAL = 4 * 1024 * 1024;
    const CHUNK = 64 * 1024;
    const socket = echo(env);
    let received = 0;
    let sinceLastPause = 0;
    let pauses = 0;
    const done = new Promise((resolve, reject) => {
      socket.on('data', (chunk) => {
        for (let i = 0; i < chunk.length; i++) {
          if (chunk[i] !== (received + i) % PATTERN_MODULUS) {
            reject(new Error(`pattern break at byte ${received + i}`));
            return;
          }
        }
        received += chunk.length;
        sinceLastPause += chunk.length;
        if (sinceLastPause >= 256 * 1024) {
          sinceLastPause = 0;
          pauses++;
          socket.pause();
          setTimeout(() => socket.resume(), 1);
        }
      });
      socket.on('end', resolve);
      socket.on('error', reject);
    });
    await once(socket, 'connect');
    for (let offset = 0; offset < TOTAL; offset += CHUNK) {
      await new Promise((resolve) =>
        socket.write(patternChunk(offset, CHUNK), resolve)
      );
    }
    socket.end();
    await done;
    strictEqual(received, TOTAL);
    ok(pauses >= 15, `${pauses} pauses`);
  },
};
