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
