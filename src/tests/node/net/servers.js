// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared machinery: connections to the sidecar servers (see tcp-servers.js)
// and small event helpers.

import net from 'node:net';
import { Buffer } from 'node:buffer';
import { ok } from 'node:assert';

function connectTo(env, portName, options = {}) {
  return net.connect({
    host: env.SIDECAR_HOSTNAME,
    port: Number(env[portName]),
    ...options,
  });
}

export const echo = (env, options) => connectTo(env, 'NET_ECHO_PORT', options);
export const endsImmediately = (env, options) =>
  connectTo(env, 'NET_END_PORT', options);
export const greet = (env, options) =>
  connectTo(env, 'NET_GREET_PORT', options);
export const sink = (env, options) => connectTo(env, 'NET_SINK_PORT', options);
export const ticker = (env, options) =>
  connectTo(env, 'NET_TICKER_PORT', options);

export const GREETING = 'hello from greet';

export function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

// Writes the segments to an echo socket one at a time, waiting for each
// one's echo — `receivedLength()` reaching the bytes sent so far — before
// sending the next, so that every segment is delivered on its own whatever
// TCP makes of the timing. Gives up, failing the test, after a few seconds.
export async function echoSegments(socket, segments, receivedLength) {
  let sent = 0;
  for (const segment of segments) {
    socket.write(segment);
    sent += Buffer.byteLength(segment);
    for (let i = 0; receivedLength() < sent; i++) {
      ok(i < 2000, `echo of ${JSON.stringify(segment)} never arrived`);
      await scheduler.wait(2);
    }
  }
}

// Resolves with the socket's 'data' concatenated until at least `length`
// bytes have arrived.
export function readAtLeast(socket, length) {
  return new Promise((resolve) => {
    const chunks = [];
    let total = 0;
    const onData = (chunk) => {
      chunks.push(chunk);
      total += chunk.length;
      if (total >= length) {
        socket.off('data', onData);
        resolve(Buffer.concat(chunks));
      }
    };
    socket.on('data', onData);
  });
}

// Resolves with the concatenation of everything the socket emits as
// 'data' until 'end'.
export function readAll(socket) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    socket.on('data', (chunk) => chunks.push(chunk));
    socket.once('end', () => resolve(chunks));
    socket.once('error', reject);
  });
}
