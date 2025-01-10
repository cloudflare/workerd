// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This file is used as a sidecar for the net-nodejs-test tests.
// It creates 2 TCP servers to act as a source of truth for the node:net tests.
// We execute this command using Node.js, which makes net.createServer available.
const net = require('node:net');

const server = net.createServer((s) => {
  s.on('error', () => {
    // Do nothing
  });
  s.end();
});
server.listen(9999, () => console.info('Listening on port 9999'));

const echoServer = net.createServer((s) => {
  s.setTimeout(100);
  s.on('error', () => {
    // Do nothing
  });
  s.pipe(s);
});
echoServer.listen(9998, () => console.info('Listening on port 9998'));

const timeoutServer = net.createServer((s) => {
  s.setTimeout(100);
  s.resume();
  s.once('timeout', () => {
    // Try to reset the timeout.
    s.write('WHAT.');
  });

  s.on('end', () => {
    s.end();
  });
  s.on('error', () => {
    // Do nothing
  });
});
timeoutServer.listen(9997, () => console.info('Listening on port 9997'));
