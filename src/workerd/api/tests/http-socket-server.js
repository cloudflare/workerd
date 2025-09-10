// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This file is used as a sidecar for the http-socket tests.
// It creates an HTTP server that will respond to requests from the convertSocketToFetcher API.
const http = require('node:http');
const crypto = require('node:crypto');
const net = require('node:net');
const tls = require('node:tls');
const assert = require('node:assert');

// Handle upgrade requests to switch from HTTP to WebSocket protocol
function upgradeToWebSocketConnection(req, socket, head) {
  // Check if it's a WebSocket upgrade request
  if (req.headers['upgrade'] !== 'websocket') {
    socket.end('HTTP/1.1 400 Bad Request');
    return;
  }

  //console.log('WebSocket upgrade request received');
  //console.log('WebSocket headers:', req.headers);

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
  //console.log(`Received request: ${req.method} ${req.url}`);

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
  } else if (req.url === '/drop') {
    res.socket.drop();
  } else if (req.url === '/destroy') {
    res.socket.destroy();
  } else if (req.url === '/redirect') {
    res.writeHead(301, { Location: '/ping' });
    res.end('Moved Permanently');
  } else {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end('Not Found');
  }
});

// Handle WebSocket upgrade requests
server.on('upgrade', upgradeToWebSocketConnection);

// Function to handle WebSocket connections
function handleWebSocketConnection(socket) {
  //console.log('WebSocket connection established');

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
        //console.log('Received WebSocket message:', message);

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
    //console.log('WebSocket connection closed');
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

// This socket grabs connections and immediately drop them
const dropServer = net.createServer((socket) => {
  socket.on('error', (err) => {
    console.log('DROP: ' + err.name);
    console.log('DROP: ' + err.message);
  });
  var ready = true;
  // Repeatedly send a page of data till the socket is borked
  const repeatedString = 'A'.repeat(4_096);
  while (ready) {
    ready = socket.write(repeatedString + '\n');
  }
  socket.write(repeatedString + '\n');
});

dropServer.listen(process.env.SOCKET_PARTIALLY_WRITTEN, () => {
  console.info(
    `Drop Socket test server listening on port ${dropServer.address().port}`
  );
});

// Flush Hello Socket server that checks for hello message and responds with HTTP pong
const flushHelloServer = net.createServer((socket) => {
  let receivedHello = false;
  let buffer = Buffer.alloc(0);

  socket.on('data', (data) => {
    buffer = Buffer.concat([buffer, data]);
    const message = buffer.toString().trim();

    if (!receivedHello && message.includes('Hello')) {
      receivedHello = true;
      // Clear the buffer after processing hello
      buffer = Buffer.alloc(0);
      return;
    }

    if (receivedHello) {
      // Respond with HTTP pong response
      const httpResponse =
        'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 4\r\n\r\npong';
      socket.write(httpResponse);
      socket.end();
    }
  });

  socket.on('error', (err) => {
    console.log('FLUSH_HELLO error:', err.message);
  });
});

flushHelloServer.listen(process.env.FLUSH_HELLO_SOCKET, () => {
  console.info(
    `Flush Hello Socket server listening on port ${flushHelloServer.address().port}`
  );
});

// Create a self-signed certificate for TLS with proper SAN extension
function createSelfSignedCert() {
  const key = `-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQDIWfGy2tRsqANt
J1F/52bIDzMDxlmSkDpu3U3Ehq6TmH2hNBcLOWuWLvG8Np9artnzk8QnodfN8yEJ
0HRzZ6mRjVIUHJOb3+L1+0ePOM8dtWvG0AOd95K0T0imJRLPR18UjHl5OLE7mMS9
CGHa6mDTGKzTcdxtpkjiyoNgfdKKSKzLplga5if36leGJ2+mEhOAc/cV1kqOx+hP
VGAOz5p3OnkgolC8hZ3WTsAFEYMU0QoPNs7jVCVNGH9t3qPiWV2/7XaNQoMnMHgq
yyljcvDo0O0dw0HKtBVIMhz5xHHGZqJNM/R36MeHzO+cIYhJz+ncknu62+IlQOCY
eyHuxvpRAgMBAAECggEAB3SXYqsze+6doAZ2SS7se3XbVWDgbOyKjB0Wm4FShkIG
rMTCNcP3fbF2A+W5dNesWxzM0Be87thFCrcz2iaJoBW07/QnRwXkDXjCDzGTPX0G
i3GqrMptbmHD55DaHBYBEwPuMkVajQfwjEM/VvThUQGqTrz+MaNeM3hLPsA34Tbl
wigHK+4tyAFLkYkvsxXYHs1F23ey2ubFUyBI8gvfayOvr4MOVfEbQZsmz3IB5jjk
oK5EaMJuhty65pb9Pi6ncSbfVQ2aciNgHjZs/is/WQfQPB2jYBPpmaFQbZc2pseZ
8zTLvF+GKZng4hQE1F+F6DUf9sxb54Eu1XqzqGlrdQKBgQDkQc5fJj72rF9RkiBN
zeEK0Ngihm9Jzsb544i7DT/4jPWwa1+dt+kNVqguDbcxi4tCi+P8BwTnfX5M31d2
/Si9nDBpP+WLLRHjFq2AQf05JvFc1/7s4KvWBrfKbwiN+uxrstB2lDuFp386B20U
stsCBu/nawjt5lYY7S0zPokkJQKBgQDgs9rTJNnRuaaEpUxqRdR6zR3z0Qe1B1Ga
y6z8OqiX3N+wM9uAcrTzGOtPKvB4glcJZrrQp0NSxkJVsFBzPBCfUfjIV/IiOqS1
nE/rrEKEG7ZVkxDUsdeS8KiVKBJjLch/hrT0udgv4vndXqeaJuNzbD1yfiKbPnY5
yGC78uqvvQKBgGUGMx6twMRQekeSEzYcXvP4hxCQy4SxPiOvbv7K2HtbeApDG6ik
k0NSDVGExIXrKxGi9J7BRIxoYJQJbZ6+YV+6VzreCuxUYExP5y6TBk5bTAw5lRym
O6eYhZPVHMYqPqVUGSvCY629+nNmggLdPk1hYKDeIK+aeJTDtHOvw+b5AoGAZI74
wf8+34WWyMv026ZuhZpf6ipEqbYhxgWaX7KcmoHFNWSvudcbtaMUQ3Sy8ytZaiKo
PhJspZGGRDTIfBmIUtRrYrVA7iKSbZgLiCuqBNcmDTvoj1cbY24B8+Zf/DSUAsY1
G0RERIHuUiw3E1yN86yf/yoFsLYOUKOk7teyQX0CgYBFUzUeVszODI/1NeKuGdoR
1uJk2gt7hH4t7GdX4G78h3i6P5pa7qSyUXqBm53nXrYhEFRiVgDh5BoRnoKKWomj
IDidHZX3fMoQvdKAT8sUbT/Q3KKWPFqGv7frdpQe+JM0DwwjjPrMtUWhuve455XW
bCKgoxoaqEPUzY9CyI+RZg==
-----END PRIVATE KEY-----`;

  const cert = `-----BEGIN CERTIFICATE-----
MIIDmTCCAoGgAwIBAgIUFdaq1aN7zHoSsmLp6ItxnM1VOPEwDQYJKoZIhvcNAQEL
BQAwTjELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRlc3QxDTALBgNVBAcMBFRlc3Qx
DTALBgNVBAoMBFRlc3QxEjAQBgNVBAMMCWxvY2FsaG9zdDAeFw0yNTA3MjYwNjI3
MDZaFw0yNjA3MjYwNjI3MDZaME4xCzAJBgNVBAYTAlVTMQ0wCwYDVQQIDARUZXN0
MQ0wCwYDVQQHDARUZXN0MQ0wCwYDVQQKDARUZXN0MRIwEAYDVQQDDAlsb2NhbGhv
c3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDIWfGy2tRsqANtJ1F/
52bIDzMDxlmSkDpu3U3Ehq6TmH2hNBcLOWuWLvG8Np9artnzk8QnodfN8yEJ0HRz
Z6mRjVIUHJOb3+L1+0ePOM8dtWvG0AOd95K0T0imJRLPR18UjHl5OLE7mMS9CGHa
6mDTGKzTcdxtpkjiyoNgfdKKSKzLplga5if36leGJ2+mEhOAc/cV1kqOx+hPVGAO
z5p3OnkgolC8hZ3WTsAFEYMU0QoPNs7jVCVNGH9t3qPiWV2/7XaNQoMnMHgqyylj
cvDo0O0dw0HKtBVIMhz5xHHGZqJNM/R36MeHzO+cIYhJz+ncknu62+IlQOCYeyHu
xvpRAgMBAAGjbzBtMB0GA1UdDgQWBBQ6R0NLwjufqWjT75cFdzHW9bh2DDAfBgNV
HSMEGDAWgBQ6R0NLwjufqWjT75cFdzHW9bh2DDAPBgNVHRMBAf8EBTADAQH/MBoG
A1UdEQQTMBGCCWxvY2FsaG9zdIcEfwAAATANBgkqhkiG9w0BAQsFAAOCAQEAToR4
CaI9HAfSSXE+6fPthp+qrwPmfx3rW0RskpjKuqemZmIK7ydU9pcYuGtc6CPen614
RfFoaWtPltbNU0KV79P4zTRNYxqTKcEyhsjyAGbLA+bJtJE3hlDrfPGVyepZETXE
7Ig4XPyXi4M+WmvLboAF2dHC+H1XoWp3agIN45VRnr5uPVNX19dTbr0gc3WxLEUH
N839sKGVB9GVaQhF/4Z8ia0bluirf+6SNAaN/veJA40ixGEkHN3gqX4ZTZWl5rji
3cLdth83wmueKxiBp8ov78ubmdPiBsyIVrsb8jEsxRPAKX8gEx09S/yIIs1ZEGi9
wG73FlfFCc09zjmYww==
-----END CERTIFICATE-----`;

  return { key, cert };
}

// STARTTLS Socket server that implements proper handshake protocol
const startTlsSocketServer = net.createServer((s) => {
  console.log('STARTTLS_SOCKET: New connection received');

  // Send initial greeting
  s.write('HELLO\n');

  // Wait for one response then upgrade to TLS
  s.once('data', (data) => {
    const response = data.toString().trim();
    console.log('STARTTLS_SOCKET: Received response:', response);

    if (response === 'HELLO_BACK') {
      s.write('START_TLS\n', () => {
        console.log('STARTTLS_SOCKET: Sent START_TLS, upgrading to TLS');

        // Small delay to ensure START_TLS is sent
        console.log('STARTTLS_SOCKET: Creating TLS socket');
        const tlsSocket = new tls.TLSSocket(s, {
          isServer: true,
          server: startTlsSocketServer,
          secureContext: tls.createSecureContext(createSelfSignedCert()),
          requestCert: false,
          SNICallback: (hostname, callback) => {
            console.log(
              'STARTTLS_SOCKET: SNI callback for hostname:',
              hostname
            );
            callback(null, null);
          },
        });

        console.log('STARTTLS_SOCKET: Setting up TLS event handlers');

        tlsSocket.on('secure', () => {
          console.log('STARTTLS_SOCKET: TLS handshake complete');

          // Handle TLS data
          tlsSocket.on('data', (data) => {
            const message = data.toString().trim();
            console.log('STARTTLS_SOCKET: Received TLS message:', message);

            if (message === 'ping') {
              console.log('STARTTLS_SOCKET: Sending pong response');
              tlsSocket.write('pong\n', (err) => {
                if (err) {
                  console.log('STARTTLS_SOCKET: Error writing pong:', err);
                } else {
                  console.log('STARTTLS_SOCKET: Pong sent successfully');
                }
              });
            } else if (message.includes('GET') || message.includes('POST')) {
              // Handle HTTP requests over TLS
              console.log('STARTTLS_SOCKET: TLS HTTP request:', message);
              const httpResponse =
                'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 4\r\n\r\npong';
              tlsSocket.write(httpResponse);
            }
          });
        });

        tlsSocket.on('error', (err) => {
          console.log('STARTTLS_SOCKET TLS error:', err.message);
        });

        tlsSocket.on('close', () => {
          console.log('STARTTLS_SOCKET: TLS socket closed');
        });

        console.log(
          'STARTTLS_SOCKET: TLS socket created, waiting for handshake'
        );

        // The TLS handshake should start when the client initiates it
      });
    }
  });

  s.on('error', (err) => {
    console.log('STARTTLS_SOCKET socket error:', err.message);
  });
});

startTlsSocketServer.listen(process.env.STARTTLS_SOCKET, () => {
  console.info(
    `STARTTLS Socket server listening on port ${startTlsSocketServer.address().port}`
  );
});
