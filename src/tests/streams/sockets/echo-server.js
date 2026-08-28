// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Sidecar TCP servers for the sockets streams suite.
//
// ECHO (STREAMS_ECHO_PORT): echoes every byte back; when the client
// half-closes, flushes and ends its side (so the client's readable
// reaches EOF after the full echo).
//
// GREET (STREAMS_GREET_PORT): writes a fixed greeting immediately and
// ends its side (client readable delivers the greeting, then EOF); the
// server keeps reading anything the client sends until close.

import net from 'node:net';

const host = process.env.SIDECAR_HOSTNAME ?? '127.0.0.1';

const echo = net.createServer((socket) => {
  socket.on('data', (data) => socket.write(data));
  socket.on('end', () => socket.end());
  socket.on('error', () => {});
});
echo.listen({ port: 0, host }, () => {
  console.log(`STREAMS_ECHO_PORT=${echo.address().port}`);
});

const GREETING = 'hello from the greet server';
const greet = net.createServer((socket) => {
  socket.write(GREETING);
  socket.end();
  socket.on('error', () => {});
});
greet.listen({ port: 0, host }, () => {
  console.log(`STREAMS_GREET_PORT=${greet.address().port}`);
});
