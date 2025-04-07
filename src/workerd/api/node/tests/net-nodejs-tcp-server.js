// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This file is used as a sidecar for the net-nodejs-test tests.
// It creates 2 TCP servers to act as a source of truth for the node:net tests.
// We execute this command using Node.js, which makes net.createServer available.
const net = require('node:net');

function reportPort(server) {
  console.info(`Listening on port ${server.address().port}`);
}

const server = net.createServer((s) => {
  s.on('error', () => {
    // Do nothing
  });
  s.end();
});
server.listen(process.env.SERVER_PORT, () => reportPort(server));

const echoServer = net.createServer((s) => {
  s.setTimeout(100);
  s.on('error', () => {
    // Do nothing
  });
  s.pipe(s);
});
echoServer.listen(process.env.ECHO_SERVER_PORT, () => reportPort(echoServer));

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
timeoutServer.listen(process.env.TIMEOUT_SERVER_PORT, () =>
  reportPort(timeoutServer)
);

const endServer = net.createServer((s) => {
  s.end();
});
endServer.listen(process.env.END_SERVER_PORT, () => reportPort(endServer));

const serverThatDies = net.createServer(function (s) {
  serverThatDies.close();
  s.end();
});
serverThatDies.listen(process.env.SERVER_THAT_DIES_PORT, () =>
  reportPort(serverThatDies)
);
