// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Sidecar TCP servers for the node:net suite. Each reports its port as
// NAME=PORT on stdout for the wd_test supervisor.
//
// ECHO (NET_ECHO_PORT): echoes every byte; when the client half-closes,
// flushes and ends its side. Never times out on its own.
//
// END (NET_END_PORT): ends its write side immediately on connect, keeps
// reading and discarding until the client ends, then closes.
//
// GREET (NET_GREET_PORT): writes one greeting and ends its write side, then
// keeps reading and discarding until the client ends.
//
// SINK (NET_SINK_PORT): reads and counts everything without writing; when
// the client ends, writes the decimal byte count and ends.
//
// TICKER (NET_TICKER_PORT): writes "tick" every 20 ms until the client
// ends, then ends.
//
// RESET (NET_RESET_PORT): writes "ready", then resets the connection (RST,
// no FIN) as soon as the first bytes from the client arrive.
//
// TRICKLE (NET_TRICKLE_PORT): writes bytes 0..1999 (i % 251) one at a
// time, one per millisecond, then ends; discards input.
//
// UTF8_SPLIT (NET_UTF8_SPLIT_PORT): writes the text of `utf8SplitText` (its
// bytes are known to the suite as UTF8_SPLIT_TEXT)
// one BYTE per write with a 1 ms gap, so that every multi-byte sequence is
// split at every boundary, then ends; discards input.

import net from 'node:net';

const host = process.env.SIDECAR_HOSTNAME ?? '127.0.0.1';

function listen(server, name) {
  server.listen({ port: 0, host }, () => {
    console.log(`${name}=${server.address().port}`);
  });
}

listen(
  net.createServer((socket) => {
    socket.on('data', (data) => socket.write(data));
    socket.on('end', () => socket.end());
    socket.on('error', () => {});
  }),
  'NET_ECHO_PORT'
);

listen(
  net.createServer((socket) => {
    socket.on('error', () => {});
    socket.resume();
    socket.end();
  }),
  'NET_END_PORT'
);

listen(
  net.createServer((socket) => {
    socket.on('error', () => {});
    socket.resume();
    socket.end('hello from greet');
  }),
  'NET_GREET_PORT'
);

listen(
  net.createServer((socket) => {
    socket.on('error', () => {});
    let count = 0;
    socket.on('data', (data) => {
      count += data.byteLength;
    });
    socket.on('end', () => socket.end(String(count)));
  }),
  'NET_SINK_PORT'
);

listen(
  net.createServer((socket) => {
    socket.on('error', () => {});
    socket.write('ready');
    socket.once('data', () => socket.resetAndDestroy());
  }),
  'NET_RESET_PORT'
);

function writeSlowly(socket, bytes) {
  let i = 0;
  const timer = setInterval(() => {
    if (socket.destroyed) {
      clearInterval(timer);
      return;
    }
    if (i === bytes.length) {
      clearInterval(timer);
      socket.end();
      return;
    }
    socket.write(bytes.subarray(i, i + 1));
    i++;
  }, 1);
  socket.on('close', () => clearInterval(timer));
}

listen(
  net.createServer((socket) => {
    socket.on('error', () => {});
    socket.resume();
    const bytes = new Uint8Array(2000);
    for (let i = 0; i < bytes.length; i++) bytes[i] = i % 251;
    writeSlowly(socket, bytes);
  }),
  'NET_TRICKLE_PORT'
);

// Two-, three- and four-byte sequences (é, €, 😀) between ASCII letters.
const utf8SplitText = 'a\u00e9b\u20acc\u{1F600}d\u00e9\u20ac\u{1F600}e';

listen(
  net.createServer((socket) => {
    socket.on('error', () => {});
    socket.resume();
    writeSlowly(socket, new TextEncoder().encode(utf8SplitText));
  }),
  'NET_UTF8_SPLIT_PORT'
);

listen(
  net.createServer((socket) => {
    socket.on('error', () => {});
    socket.resume();
    const timer = setInterval(() => socket.write('tick'), 20);
    socket.on('end', () => {
      clearInterval(timer);
      socket.end();
    });
    socket.on('close', () => clearInterval(timer));
  }),
  'NET_TICKER_PORT'
);
