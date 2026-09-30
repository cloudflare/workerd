// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// WebSocket server for websocket-open-error-test. After the handshake it waits for the first
// frame from the client, stops reading, replies with a single text frame, and then resets the
// TCP connection (no Close frame) while the client is still writing.

const http = require('http');
const crypto = require('crypto');

function textFrame(text) {
  const payload = Buffer.from(text);
  // Short (<= 125 byte) unmasked text frame with FIN set.
  return Buffer.concat([Buffer.from([0x81, payload.length]), payload]);
}

const server = http.createServer((req, res) => {
  res.writeHead(404);
  res.end();
});

server.on('upgrade', (req, socket) => {
  const acceptKey = crypto
    .createHash('sha1')
    .update(
      req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
    )
    .digest('base64');

  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${acceptKey}\r\n` +
      '\r\n'
  );

  socket.on('error', () => {});
  socket.once('data', () => {
    // Stop reading, so that the client's large follow-up message stalls in its outgoing pump.
    socket.pause();
    socket.write(textFrame('pong'));
    // Destroying the socket with unread data resets the connection, failing the client's
    // in-flight write.
    setTimeout(() => socket.destroy(), 300);
  });
});

server.listen({ port: 0, host: process.env.SIDECAR_HOSTNAME }, () => {
  console.log(`OPEN_ERROR_SERVER_PORT=${server.address().port}`);
});
