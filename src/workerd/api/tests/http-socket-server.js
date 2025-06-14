// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This file is used as a sidecar for the http-socket tests.
// It creates an HTTP server that will respond to requests from the convertSocketToFetcher API.
const http = require('http');
const crypto = require('crypto');

// Handle upgrade requests to switch from HTTP to WebSocket protocol
function upgradeToWebSocketConnection(req, socket, head) {
  // Check if it's a WebSocket upgrade request
  if (req.headers['upgrade'] !== 'websocket') {
    socket.end('HTTP/1.1 400 Bad Request');
    return;
  }

  console.log('WebSocket upgrade request received');
  console.log('WebSocket headers:', req.headers);

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

// Parse WebSocket frames to extract message data
function parseWebSocketFrame(buffer) {
  if (buffer.length < 2) return null;

  const isFinalFrame = !!(buffer[0] & 0x80);
  const opcode = buffer[0] & 0x0f;
  const isMasked = !!(buffer[1] & 0x80);
  let payloadLength = buffer[1] & 0x7f;

  let maskingKeyOffset = 2;
  if (payloadLength === 126) {
    payloadLength = buffer.readUInt16BE(2);
    maskingKeyOffset = 4;
  } else if (payloadLength === 127) {
    payloadLength = Number(buffer.readBigUInt64BE(2));
    maskingKeyOffset = 10;
  }

  if (!isMasked) {
    // According to the spec, client messages must be masked
    return {
      opcode,
      payload: Buffer.alloc(0),
      isControl: (opcode & 0x8) !== 0,
    };
  }

  const maskingKey = buffer.slice(maskingKeyOffset, maskingKeyOffset + 4);
  const payloadOffset = maskingKeyOffset + 4;

  if (buffer.length < payloadOffset + payloadLength) {
    return null; // Not enough data
  }

  const payload = Buffer.alloc(payloadLength);
  for (let i = 0; i < payloadLength; i++) {
    payload[i] = buffer[payloadOffset + i] ^ maskingKey[i % 4];
  }

  return {
    opcode,
    payload,
    isControl: (opcode & 0x8) !== 0,
  };
}

// Create HTTP server
const server = http.createServer((req, res) => {
  console.log(`Received request: ${req.method} ${req.url}`);

  if (req.url === '/ping') {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('pong');
  } else if (req.url === '/json') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ message: 'Hello from HTTP socket server' }));
  } else if (req.url === '/echo' && req.method === 'POST') {
    let body = '';
    req.on('data', (chunk) => {
      body += chunk.toString();
    });
    req.on('end', () => {
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.end(body);
    });
  } else if (req.url === '/headers') {
    const headers = {};
    for (const [key, value] of Object.entries(req.headers)) {
      headers[key] = value;
    }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(headers));
  } else if (req.url === '/status/404') {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end('Not Found');
  } else if (req.url === '/status/500') {
    res.writeHead(500, { 'Content-Type': 'text/plain' });
    res.end('Internal Server Error');
  } else {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end('Not Found');
  }
});

// Handle WebSocket upgrade requests
server.on('upgrade', upgradeToWebSocketConnection);

// Function to handle WebSocket connections
function handleWebSocketConnection(socket) {
  console.log('WebSocket connection established');

  // Send a welcome message
  sendMessage(socket, 'Welcome to WebSocket server');

  // Handle incoming data
  let buffer = Buffer.alloc(0);
  socket.on('data', (data) => {
    buffer = Buffer.concat([buffer, data]);

    // Process frames until we can't anymore
    let frame;
    while ((frame = parseWebSocketFrame(buffer))) {
      // Handle different frame types
      if (frame.isControl) {
        if (frame.opcode === 0x8) {
          // Close frame
          socket.end();
          return;
        }
        // Skip other control frames
        continue;
      }

      // For text/binary frames, echo the message back
      if (frame.opcode === 0x1) {
        // Text frame
        const message = frame.payload.toString('utf8');
        console.log('Received WebSocket message:', message);

        // Echo the message back
        sendMessage(socket, `Echo: ${message}`);
      }

      // Remove processed frame from buffer
      const frameSize =
        frame.payload.length + (frame.payload.length > 125 ? 4 : 2) + 4; // header + masking key + payload
      buffer = buffer.slice(frameSize);
    }
  });

  // Handle socket close
  socket.on('end', () => {
    console.log('WebSocket connection closed');
  });

  socket.on('error', (error) => {
    console.error('WebSocket error:', error);
  });
}

server.listen(process.env.HTTP_SOCKET_SERVER_PORT, () => {
  console.info(
    `HTTP Socket test server listening on port ${server.address().port}`
  );
});
