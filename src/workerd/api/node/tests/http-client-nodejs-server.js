// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This is a sidecar that runs alongside the http-client-nodejs-test.* tests.
// It is executed using the appropriate Node.js version defined in build/deps/nodejs.MODULE.bazel.
const http = require('node:http');
const assert = require('node:assert/strict');
const zlib = require('node:zlib');

function listenAndReport(server, name) {
  server.listen({ port: 0, host: process.env.SIDECAR_HOSTNAME }, () => {
    console.log(`${name}=${server.address().port}`);
  });
}

const pongServer = http.createServer((req, res) => {
  req.resume();
  req.on('end', () => {
    res.writeHead(200);
    res.end('pong');
  });
});

listenAndReport(pongServer, 'PONG_SERVER_PORT');

const asdServer = http.createServer((_req, res) => {
  res.end('asd');
});

listenAndReport(asdServer, 'ASD_SERVER_PORT');

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

  listenAndReport(defaultHeadersExistServer, 'DEFAULT_HEADERS_EXIST_PORT');
}

const requestArgumentsServer = http.createServer((req, res) => {
  assert.strictEqual(req.url, '/testpath');
  res.end();
});

listenAndReport(requestArgumentsServer, 'REQUEST_ARGUMENTS_PORT');

const helloWorldServer = http.createServer((req, res) => {
  res.removeHeader('Date');
  res.setHeader('Keep-Alive', 'timeout=1');

  switch (req.url.slice(1).split('?').at(0)) {
    case 'join-duplicate-headers': {
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
    }

    case 'search-path': {
      res.writeHead(200, { 'Content-Type': 'text/plain', url: req.url });
      res.end('Hello, World!');
      break;
    }

    case 'echo': {
      // Echo the request body back as the response
      let body = '';
      req.on('data', (chunk) => {
        body += chunk.toString();
      });
      req.on('end', () => {
        res.writeHead(200, { 'Content-Type': 'text/plain' });
        res.end(body);
      });
      break;
    }

    default: {
      res.end();
    }
  }
});

listenAndReport(helloWorldServer, 'HELLO_WORLD_SERVER_PORT');

const gzipServer = http.createServer((_req, res) => {
  const body = zlib.gzipSync(Buffer.from('hello from gzip server'));
  res.writeHead(200, {
    'Content-Encoding': 'gzip',
    'Content-Type': 'text/plain',
  });
  res.end(body);
});

listenAndReport(gzipServer, 'GZIP_SERVER_PORT');

// Echoes back the Host header the sidecar received, so the test can verify
// that a user-supplied Host header does not redirect the transport destination.
const hostEchoServer = http.createServer((req, res) => {
  req.resume();
  req.on('end', () => {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end(req.headers.host || '');
  });
});

listenAndReport(hostEchoServer, 'HOST_ECHO_SERVER_PORT');
