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

server.listen(0, () => {
  console.log(`HTTP_SOCKET_SERVER_PORT=${server.address().port}`);
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

dropServer.listen(0, () => {
  console.log(`SOCKET_PARTIALLY_WRITTEN=${dropServer.address().port}`);
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

flushHelloServer.listen(0, () => {
  console.log(`FLUSH_HELLO_SOCKET=${flushHelloServer.address().port}`);
});

// Create a self-signed certificate for TLS with proper SAN extension
function createSelfSignedCert() {
  const key = `-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCmi4tGNwMie1Ha
IYThyrfGHUYFTrgWXTSmqNHVaJYNc0DccJBb5+COO3XvN21hkLyaq7/BLGxZuDRf
ZUDbrD7Ljlp37UlT9elnjBIUTkciVCSuO8WWZxXCGHJBOXz0Lu+Yn8JSaf0haBmW
k7NBiM1q7y7iyArXb2vxWhgwesJoxqqQAKhOaK+w46qK1LRzAps5nKQSPQ4s6TTV
nEKH8gPSbb8CO5mZi31yX2gTmdnNkcaCkGZpMzfTvM1WhcFEWiIxgpuzzbJ6bM8g
aD4PHZtH5py5/tq1qJn0SVWV+5APRGHL2wuRmOincJJ/Z7gaLEwzJYfAON1RY4nW
yqJ45PNxAgMBAAECggEAClZ3GrCnfShyeDvEZ8+RgLaodgPg4WJ/tiicfca0KbmR
uR0zmMDX63w9Yc/q9jdpvaATkwYS467PcWxzji8u5qu7ad5Mwu1sw4SqSqBhQfw/
GqGTLjbP3vBthybuXqnbLXodMchqcfxoOjSI1/SJ661VF7abFaxQ4vZYsgmsr5wr
r9DL4ZXl0v5+vQciThfwnfPhLOVj8yQQsYP9YFR147aMPVDbw6V34+xTWOlr8g1s
0pThbq+4J2vMorZL/T5NaiAAxtQNaXfOth4Gmo88yrVUKjCNG8tOQ23iFOe8JEna
r3IrkeS5ppncGf219D3NnLZvVhRKRv99gHPc020/EQKBgQDPQ1V/bMuUdLYB4kzK
/GcCePrtIqgTLtxTNTypFLUijacwU+U4AiLBrkejpuK4DrvNc8lwhYP4lLgeC8ll
kRHOf57jymNhZs1Z4JXtxNCaX8EGNDH7xZiszlzBzuOVNN9ghN3OWuzU2Mt6GZJf
hiAv0g6rsEBmG1F+WRSZ7Lv6/wKBgQDNtRowCAPYeRMefmAum/kQ9LTmdlfCUmA2
GusfWHCYQjsbZseWa8irEJo0XM79yFChtdAFhfjSjpCjTZJnDoNQZUb3Ks9+SmvY
De83VUK5S7HeCqntAyq7IqR9c2h28zB1vf/ReRueS8fqFy612Ab1tSk7bqQd5Sgq
NF/dS9dBjwKBgHkfNWi5EKOaLP8e25fINv6X6rQIC8biHLm3o9J/mnct5uV0McEw
ZlVfXthBX78GRTQElVTfgccUSiCs7K4hQBG4PQeLr9Ys+Jasi5Ge8fU1Ph09BXTH
/bgHBOfx2sfIVT4Xh3PfaQXeB9M7/HE8dbTcgdxNrOS+1DoNHt/xG83LAoGADLfp
ypZ0RmoV+IivwbH7EEVQ+f7PJkCZmj7H0sRREdjmdqdAJ9i6K3l3T019rss30QfA
uNazr7EI2E/vgVewXsQkQxvugExxpoYWCEHJQlOfx665GuJbPf7CVM9R4ijfEiiR
LI5kWsEstxh/1tZod8CfsAEDPKXyecmLM8+Am5kCgYEAlc5G3XbbUHWKiKUZtGFc
WBtRdLX3UwF7Q2e3HYBS9uf1LtZuBPP4Xx/kwxS54R4xzXuBMnKLL3/S4+5YWZ8Q
HP4QxJfOyECHCRXLYQ+uViLWcjAs8u1IOtVEadzeITTm7WtKhfulJaEYuHhOdfs7
khOrIfcJKK8Dqqza/3yLPbc=
-----END PRIVATE KEY-----`;

  const cert = `-----BEGIN CERTIFICATE-----
MIIDmzCCAoOgAwIBAgIUcHX4QyPPWolmDciVOKeleSmshL8wDQYJKoZIhvcNAQEL
BQAwTjELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRlc3QxDTALBgNVBAcMBFRlc3Qx
DTALBgNVBAoMBFRlc3QxEjAQBgNVBAMMCWxvY2FsaG9zdDAgFw0yNjA3MjcwNDE1
NTNaGA8yMTI2MDcwMzA0MTU1M1owTjELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRl
c3QxDTALBgNVBAcMBFRlc3QxDTALBgNVBAoMBFRlc3QxEjAQBgNVBAMMCWxvY2Fs
aG9zdDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAKaLi0Y3AyJ7Udoh
hOHKt8YdRgVOuBZdNKao0dVolg1zQNxwkFvn4I47de83bWGQvJqrv8EsbFm4NF9l
QNusPsuOWnftSVP16WeMEhRORyJUJK47xZZnFcIYckE5fPQu75ifwlJp/SFoGZaT
s0GIzWrvLuLICtdva/FaGDB6wmjGqpAAqE5or7DjqorUtHMCmzmcpBI9DizpNNWc
QofyA9JtvwI7mZmLfXJfaBOZ2c2RxoKQZmkzN9O8zVaFwURaIjGCm7PNsnpszyBo
Pg8dm0fmnLn+2rWomfRJVZX7kA9EYcvbC5GY6Kdwkn9nuBosTDMlh8A43VFjidbK
onjk83ECAwEAAaNvMG0wHQYDVR0OBBYEFJ61qsbjnOYlrUGSvrnP1wMNANJOMB8G
A1UdIwQYMBaAFJ61qsbjnOYlrUGSvrnP1wMNANJOMBoGA1UdEQQTMBGCCWxvY2Fs
aG9zdIcEfwAAATAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQBx
v55hVThgDaxEHvgwHm1rfc0JdNQBxICOuEpIVj/epe++kaSQos+pEKsl/hBJG/pC
13hAyF+Ya9s5tyPinKbuFLZRyhdMS1O1zQrWvzZq3pjRRPt5ogvbwmKYGonMQnnp
8MpJa09bfdk5ZaIVO2THhtHjhzpYpKcGW4khXzCPmNTtX7Kiur0fhfkI0Ru4HVI+
BIemgjJPd01kGo1v0IK8FncbRuNDU6bDVAHinsOvSU+z2lA+cy2mN0/n44INwkHc
Zq+ggAjGTjKLo/tR37gabSUA20BKrpf5mqkMkoMRuBQ+aFdJmSNMZBR3z842nyE3
01KcXSMf3w+PAyt8iNZm
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

startTlsSocketServer.listen(0, () => {
  console.log(`STARTTLS_SOCKET=${startTlsSocketServer.address().port}`);
});
