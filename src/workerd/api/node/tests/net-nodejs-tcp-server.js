// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This file is used as a sidecar for the net-nodejs-test tests.
// It creates 2 TCP servers to act as a source of truth for the node:net tests.
// We execute this command using Node.js, which makes net.createServer available.
const net = require('node:net');

function listenAndReport(server, name) {
  server.listen({ port: 0, host: process.env.SIDECAR_HOSTNAME }, () => {
    console.log(`${name}=${server.address().port}`);
  });
}

const server = net.createServer((s) => {
  s.on('error', () => {
    // Do nothing
  });
  s.end();
});
listenAndReport(server, 'SERVER_PORT');

const echoServer = net.createServer((s) => {
  s.setTimeout(100);
  s.on('error', () => {
    // Do nothing
  });
  s.pipe(s);
});
listenAndReport(echoServer, 'ECHO_SERVER_PORT');

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
listenAndReport(timeoutServer, 'TIMEOUT_SERVER_PORT');

const endServer = net.createServer((s) => {
  s.end();
});
listenAndReport(endServer, 'END_SERVER_PORT');

let count = 0;
const serverThatDies = net.createServer(function (s) {
  // We ignore the first event because wd_test checks for the connected state
  // while preparing the sidecar test suite.
  if (count++ > 0) {
    serverThatDies.close();
  }
  s.end();
});
listenAndReport(serverThatDies, 'SERVER_THAT_DIES_PORT');

const reconnectServer = net.createServer((s) => {
  s.resume();
  s.on('error', () => {
    // Do nothing
  });
  s.write('hello\r\n');
  s.on('end', () => {
    s.end();
  });
});
listenAndReport(reconnectServer, 'RECONNECT_SERVER_PORT');
