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

{
  const expectedHeaders = {
    DELETE: ['content-length', 'host'],
    GET: ['host'],
    HEAD: ['host'],
    OPTIONS: ['content-length', 'host'],
    POST: ['content-length', 'host'],
    PUT: ['content-length', 'host'],
    TRACE: ['content-length', 'host'],
  };

  const defaultHeadersExistServer = http.createServer((req, res) => {
    res.end();

    assert(
      Object.hasOwn(expectedHeaders, req.method),
      `${req.method} was an unexpected method`
    );

    const requestHeaders = Object.keys(req.headers);
    for (const header of requestHeaders) {
      assert.ok(
        expectedHeaders[req.method].includes(header.toLowerCase()),
        `${header} should not exist for method ${req.method}`
      );
    }

    assert.deepStrictEqual(
      requestHeaders,
      expectedHeaders[req.method],
      `Unexpected headers for method ${req.method}`
    );
  });
  defaultHeadersExistServer.listen(process.env.DEFAULT_HEADERS_EXIST_PORT, () =>
    reportPort(defaultHeadersExistServer)
  );
}

const requestArgumentsServer = http.createServer((req, res) => {
  assert.strictEqual(req.url, '/testpath');
  res.end();
});
requestArgumentsServer.listen(process.env.REQUEST_ARGUMENTS_PORT, () =>
  reportPort(requestArgumentsServer)
);

const helloWorldServer = http.createServer((req, res) => {
  res.removeHeader('Date');
  res.setHeader('Keep-Alive', 'timeout=1');

  switch (req.url.slice(1)) {
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
