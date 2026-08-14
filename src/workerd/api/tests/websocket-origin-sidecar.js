// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A minimal WebSocket server that sends one text message to every client that connects. Used to
// check the origin reported on the message event of a URL-backed WebSocket, which needs a real
// connection: a WebSocketPair endpoint has no URL to take an origin from.

const http = require('http');
const crypto = require('crypto');

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function sendTextMessage(socket, message) {
  const payload = Buffer.from(message);
  // Payloads here are tiny, so the 7-bit length form is always enough.
  const header = Buffer.alloc(2);
  header[0] = 0x81; // FIN + text opcode
  header[1] = payload.length;
  socket.write(Buffer.concat([header, payload]));
}

function sendCloseFrame(socket, code) {
  const frame = Buffer.alloc(4);
  frame[0] = 0x88; // FIN + close opcode
  frame[1] = 2;
  frame.writeUInt16BE(code, 2);
  socket.write(frame);
}

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

  sendTextMessage(socket, 'hello');

  // Close from this end once the message is out. Nothing here reads the client's frames, so
  // without this the connection would stay open and the test would hang waiting on it.
  sendCloseFrame(socket, 1000);
  socket.end();
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
  console.log(`ORIGIN_SERVER_PORT=${server.address().port}`);
});
