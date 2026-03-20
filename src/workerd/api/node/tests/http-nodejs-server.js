// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This is a sidecar that runs alongside the http-client-nodejs-test.* tests.
// It is executed using the appropriate Node.js version defined in build/deps/nodejs.MODULE.bazel.
const http = require('node:http');
const assert = require('node:assert/strict');

function reportPort(server) {
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
pongServer.listen(
  process.env.PONG_SERVER_PORT,
  process.env.SIDECAR_HOSTNAME,
  () => reportPort(pongServer)
);

const asdServer = http.createServer((_req, res) => {
  res.end('asd');
});
asdServer.listen(
  process.env.ASD_SERVER_PORT,
  process.env.SIDECAR_HOSTNAME,
  () => reportPort(asdServer)
);

const timeoutServer = http.createServer((_req, res) => {
  setTimeout(() => {
    res.end('pong');
  }, 1000);
});
timeoutServer.listen(
  process.env.TIMEOUT_SERVER_PORT,
  process.env.SIDECAR_HOSTNAME,
  () => reportPort(timeoutServer)
);

const helloWorldServer = http.createServer((req, res) => {
  res.removeHeader('Date');
  res.setHeader('Keep-Alive', 'timeout=1');

  switch (req.url.slice(1)) {
    case 'multiple-writes':
      res.write('hello');
      res.end('world');
      break;
    case 'end-with-data':
      res.end('hello world');
      break;
    case 'content-length0':
      res.writeHead(200, { 'Content-Length': '0' });
      res.end();
      break;
    default:
      res.end();
  }
});
helloWorldServer.listen(
  process.env.HELLO_WORLD_SERVER_PORT,
  process.env.SIDECAR_HOSTNAME,
  () => reportPort(helloWorldServer)
);

let headerValidationServerCount = 0;
const headerValidationServer = http.createServer((req, res) => {
  if (headerValidationServerCount++ !== 0) {
    switch (req.url.slice(1)) {
      case 'test-1':
        assert.deepStrictEqual(req.rawHeaders, [
          'test',
          'value',
          'Host',
          `${process.env.SIDECAR_HOSTNAME}:${headerValidationServer.address().port}`,
          'foo',
          'bar',
          'foo',
          'baz',
          'connection',
          'close',
        ]);
        break;
      case 'test-2':
        assert.deepStrictEqual(req.rawHeaders, [
          'Content-Length',
          '0',
          'Host',
          `${process.env.SIDECAR_HOSTNAME}:${headerValidationServer.address().port}`,
        ]);
        break;
      default:
        break;
    }
  }

  res.end('ok');
});
headerValidationServer.listen(
  process.env.HEADER_VALIDATION_SERVER_PORT,
  process.env.SIDECAR_HOSTNAME,
  () => reportPort(headerValidationServer)
);
