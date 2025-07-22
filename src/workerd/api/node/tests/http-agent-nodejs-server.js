// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This is a sidecar that runs alongside the http-client-nodejs-test.* tests.
// It is executed using the appropriate Node.js version defined in WORKSPACE.
const http = require('node:http');

function reportAddress(server) {
  const address = server.address();
  console.info(`Listening on ${address.address}:${address.port}`);
}

const pongServer = http.createServer((req, res) => {
  req.resume();
  req.on('end', () => {
    res.writeHead(200);
    res.end('pong');
  });
});
pongServer.listen(process.env.PONG_SERVER_PORT, process.env.SIDECAR_HOST, () =>
  reportAddress(pongServer)
);
