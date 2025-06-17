// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This is a sidecar that runs alongside the http-client-nodejs-test.* tests.
// It is executed using the appropriate Node.js version defined in WORKSPACE.
const http = require('node:http');
const assert = require('node:assert/strict');

function reportPort(server) {
  console.info(`Listening on port ${server.address().port}`);
}

const pongServer = http.createServer((req, res) => {
  req.resume();
  req.on('end', () => {
    res.writeHead(200);
    res.end('pong');
  });
});
pongServer.listen(process.env.PONG_SERVER_PORT, () => reportPort(pongServer));

const asdServer = http.createServer((_req, res) => {
  res.end('asd');
});
asdServer.listen(process.env.ASD_SERVER_PORT, () => reportPort(asdServer));

let timeoutServerRequestCount = 0;
const timeoutServer = http.createServer((_req, res) => {
  res.flushHeaders();
  if (timeoutServerRequestCount++ === 0) {
    res.end();
  }
});
timeoutServer.listen(process.env.TIMEOUT_SERVER_PORT, () =>
  reportPort(timeoutServer)
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
    case 'join-duplicate-headers':
      res.writeHead(200, [
        'authorization',
        '3',
        'authorization',
        '4',
        'cookie',
        'foo',
        'cookie',
        'bar',
      ]);
      res.end();
      break;
    default:
      res.end();
  }
});
helloWorldServer.listen(process.env.HELLO_WORLD_SERVER_PORT, () =>
  reportPort(helloWorldServer)
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
          `localhost:${headerValidationServer.address().port}`,
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
          `localhost:${headerValidationServer.address().port}`,
        ]);
        break;
      default:
        break;
    }
  }

  res.end('ok');
});
headerValidationServer.listen(process.env.HEADER_VALIDATION_SERVER_PORT, () =>
  reportPort(headerValidationServer)
);
