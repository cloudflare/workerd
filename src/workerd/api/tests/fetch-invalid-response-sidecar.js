// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Answers every request with a response whose header value contains a NUL byte, so that the
// response head doesn't parse.

const net = require('net');

const server = net.createServer((socket) => {
  let request = '';
  socket.on('data', (chunk) => {
    request += chunk.toString('latin1');
    if (request.includes('\r\n\r\n')) {
      socket.end('HTTP/1.1 200 OK\r\nX-Bad: x\0x\r\nContent-Length: 0\r\n\r\n');
    }
  });
  socket.on('error', () => {});
});

server.listen({ port: 0, host: process.env.SIDECAR_HOSTNAME }, () => {
  console.log(`INVALID_RESPONSE_SERVER_PORT=${server.address().port}`);
});
