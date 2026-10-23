// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// WebSocket server for websocket-open-error-test. The default endpoint resets TCP while the
// client is writing. /peer-close sends Close and /peer-silent sends nothing; both keep reading
// without closing TCP. /closed waits for the corresponding client to disconnect.

const http = require('http');
const crypto = require('crypto');

function textFrame(text) {
  const payload = Buffer.from(text);
  // Short (<= 125 byte) unmasked text frame with FIN set.
  return Buffer.concat([Buffer.from([0x81, payload.length]), payload]);
}

const disconnections = new Map();

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const disconnected = disconnections.get(url.searchParams.get('id'));
  if (url.pathname !== '/closed' || !disconnected) {
    res.writeHead(404);
    res.end();
    return;
  }

  // Bound the wait, but never initiate TCP shutdown: only a client disconnect proves that
  // the native socket was released while the worker's context is still alive.
  let timer;
  const result = await Promise.race([
    disconnected,
    new Promise((resolve) => {
      timer = setTimeout(() => resolve({ closed: false }), 3000);
    }),
  ]);
  clearTimeout(timer);
  disconnections.delete(url.searchParams.get('id'));
  res.writeHead(result.closed ? 200 : 504, {
    'Content-Type': 'application/json',
  });
  res.end(JSON.stringify(result));
});

function keepOpenUntilClientDisconnects(socket, id, head, sendCloseFrame) {
  // HTTP upgrade sockets allow half-open TCP connections. Complete shutdown only after the
  // client sends EOF, so that 'close' observes client-initiated shutdown rather than waiting
  // indefinitely for the sidecar to close its own half.
  socket.once('end', () => socket.end());
  let closeSent = false;
  disconnections.set(
    id,
    new Promise((resolve) => {
      socket.once('close', () => resolve({ closed: true, closeSent }));
    })
  );
  const onData = () => {
    if (!sendCloseFrame || closeSent) return;
    closeSent = true;
    // Unmasked Close frame with status 1000.
    socket.write(Buffer.from([0x88, 0x02, 0x03, 0xe8]));
  };
  // Keep reading so that the client's outgoing pump can finish without a network error.
  socket.on('data', onData);
  if (head.length > 0) onData();
}

server.on('upgrade', (req, socket, head) => {
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
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname === '/peer-close' || url.pathname === '/peer-silent') {
    keepOpenUntilClientDisconnects(
      socket,
      url.searchParams.get('id'),
      head,
      url.pathname === '/peer-close'
    );
    return;
  }
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
