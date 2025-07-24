// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

const http = require('http');
const crypto = require('crypto');

// Handle upgrade requests to switch from HTTP to WebSocket protocol
function upgradeToWebSocketConnection(req, socket, head) {
  // Check if it's a WebSocket upgrade request
  if (req.headers['upgrade'] !== 'websocket') {
    socket.end('HTTP/1.1 400 Bad Request');
    return;
  }

  // Get the WebSocket key from the client
  const webSocketKey = req.headers['sec-websocket-key'];
  const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'; // WebSocket protocol GUID

  // Create the accept key by concatenating the key and GUID, then hashing
  const acceptKey = crypto
    .createHash('sha1')
    .update(webSocketKey + GUID)
    .digest('base64');

  // Write WebSocket handshake response headers
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${acceptKey}\r\n` +
      '\r\n'
  );

  // Socket is now a WebSocket connection
  handleWebSocketConnection(socket);
}

// Function to send a message to the client
function sendMessage(socket, message) {
  const payload = Buffer.from(message);
  const payloadLength = payload.length;

  // Create frame header
  let header;

  if (payloadLength <= 125) {
    header = Buffer.alloc(2);
    header[1] = payloadLength;
  } else if (payloadLength <= 65535) {
    header = Buffer.alloc(4);
    header[1] = 126;
    header.writeUInt16BE(payloadLength, 2);
  } else {
    header = Buffer.alloc(10);
    header[1] = 127;
    // Write length as 64-bit integer (simplified here)
    header.writeBigUInt64BE(BigInt(payloadLength), 2);
  }

  // Set the first byte: FIN bit (0x80) + opcode 0x01 for text data
  header[0] = 0x81;

  // Combine header and payload
  const frame = Buffer.concat([header, payload]);

  // Send the frame
  socket.write(frame);
}

// Get the port and host from environment variables
const port = process.env.BIG_MESSAGE_SERVER_PORT;
const host = process.env.SIDECAR_HOSTNAME;

function reportAddress(server) {
  const address = server.address();
  console.info(`Listening on ${address.address}:${address.port}`);
}

// Create HTTP server to handle the WebSocket handshake
const server = http.createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.end('WebSocket server is running');
});
server.on('upgrade', upgradeToWebSocketConnection);
server.on('error', (event) => {
  console.log(event);
  console.log(event.message);
});
// Start the server
server.listen(port, host, () => reportAddress(server));

// Function to handle WebSocket connections
function handleWebSocketConnection(socket) {
  // Send a welcome message
  sendMessage(socket, new Uint8Array(2 * 1024 * 1024));
}
