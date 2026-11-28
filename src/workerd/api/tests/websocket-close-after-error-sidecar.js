// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A WebSocket server that completes the handshake and then sends one text frame with the RSV2
// bit set. No extension gives RSV2 a meaning, so the client must fail the connection
// (RFC 6455 section 5.2).

const http = require('http');
const crypto = require('crypto');

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function upgradeToWebSocketConnection(req, socket) {
  if (req.headers['upgrade'] !== 'websocket') {
    socket.end('HTTP/1.1 400 Bad Request');
    return;
  }
  const acceptKey = crypto
    .createHash('sha1')
    .update(req.headers['sec-websocket-key'] + GUID)
    .digest('base64');
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${acceptKey}\r\n` +
      '\r\n'
  );

  // FIN (0x80) + RSV2 (0x20) + text opcode (0x01), then a two-byte payload.
  socket.write(Buffer.from([0xa1, 0x02, 0x68, 0x69]));
  // Keep the TCP connection open so the client's failure, not a disconnect, ends its read loop.
}

const server = http.createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.end('WebSocket server is running');
});
server.on('upgrade', upgradeToWebSocketConnection);
server.on('error', (err) => {
  console.log(err.message);
});
server.listen({ port: 0, host: process.env.SIDECAR_HOSTNAME }, () => {
  console.log(`RSV_SERVER_PORT=${server.address().port}`);
});
