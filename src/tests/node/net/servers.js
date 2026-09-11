// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared machinery: connections to the sidecar servers (see tcp-servers.js)
// and small event helpers.

import net from 'node:net';

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
