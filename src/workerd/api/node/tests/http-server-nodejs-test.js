import http from 'node:http';
import { WorkerEntrypoint } from 'cloudflare:workers';
import { strictEqual, ok, throws, notStrictEqual, rejects } from 'node:assert';
import { httpServerHandler, handleAsNodeRequest } from 'cloudflare:node';
import { mock } from 'node:test';
import url from 'node:url';
import qs from 'node:querystring';

export const checkPortsSetCorrectly = {
  test(_ctrl, env) {
    const keys = ['PONG_SERVER_PORT'];
    for (const key of keys) {
      strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
  },
};

export class GlobalService extends WorkerEntrypoint {
  async fetch(request) {
    await rejects(handleAsNodeRequest({}, request), {
      message: /Failed to determine port for server/,
    });
    await rejects(handleAsNodeRequest(1234, request), {
      message: /^Http server with port 1234 not found/,
    });
    return await handleAsNodeRequest({ port: 9090 }, request);
  }
}

export const testHttpServerHandler = {
  test() {
    throws(() => httpServerHandler(null), {
      message: /Server descriptor cannot be null or undefined/,
    });

    throws(() => httpServerHandler({}), {
      message: /Failed to determine port for server/,
    });
  },
};

const globalServer = http.createServer((req, res) => {
  res.writeHead(200);
  res.end('Hello, World!');
});

globalServer.listen(9090);

export const testGlobalHttpServe = {
  async test(_ctrl, env) {
    strictEqual(globalServer.listening, true);
    strictEqual(globalServer.address().port, 9090);

    const res = await env.GLOBAL_SERVICE.fetch('https://cloudflare.com');
    strictEqual(res.status, 200);
    strictEqual(await res.text(), 'Hello, World!');

    globalServer.close();
  },
};

// Test is taken from test/parallel/test-http-server-async-dispose.js
export const testHttpServerAsyncDispose = {
  async test() {
    const server = http.createServer();

    server.listen(8080);
    ok(server.listening);
    const closeFn = mock.fn();
    server.on('close', closeFn);
    await server[Symbol.asyncDispose]();
    ok(!server.listening);
    strictEqual(closeFn.mock.callCount(), 1);
  },
};

// Test is taken from test/parallel/test-http-server-incomingmessage-destroy.js
export const testHttpServerIncomingMessageDestroy = {
  async test(_ctrl, env) {
    const onErrorFn = mock.fn();
    await using server = http.createServer((req, res) => {
      const path = req.url;

      ok('cloudflare' in req);
      ok('cf' in req.cloudflare);

      if (path === '/destroy-with-error') {
        req.on('error', (err) => {
          res.statusCode = 400;
          res.end('Request destroyed: ' + err.message);
        });
        req.destroy(new Error('Destroy test'));
      } else if (path === '/destroy-without-error') {
        req.once('error', onErrorFn);
        req.on('close', () => {
          res.statusCode = 200;
          res.end('Request destroyed without error');
        });
        req.destroy();
      } else if (path === '/response-destroy-with-error') {
        res.destroy(new Error('Response destroy test'));
      }
    });

    server.listen(8080);

    {
      const res = await env.SERVICE.fetch(
        'https://cloudflare.com/destroy-with-error'
      );
      strictEqual(res.status, 400);
      strictEqual(await res.text(), 'Request destroyed: Destroy test');
    }

    {
      const res = await env.SERVICE.fetch(
        'https://cloudflare.com/destroy-without-error'
      );
      strictEqual(res.status, 200);
      strictEqual(await res.text(), 'Request destroyed without error');
      strictEqual(onErrorFn.mock.callCount(), 0);
    }
  },
};

// Test is taken from test/parallel/test-http-server-method.query.js
export const testHttpServerMethodQuery = {
  async test(_ctrl, env) {
    await using server = http.createServer((req, res) => {
      strictEqual(req.method, 'QUERY');
      res.end('OK');
    });
    server.listen(8080);

    const res = await env.SERVICE.fetch('https://cloudflare.com', {
      method: 'QUERY',
    });
    strictEqual(res.status, 200);
    strictEqual(await res.text(), 'OK');
  },
};

// Tests is taken from test/parallel/test-http-server-multiheaders.js
export const testHttpServerMultiHeaders = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    const server = http.createServer(function (req, res) {
      strictEqual(req.headers.accept, 'abc, def, ghijklmnopqrst');
      strictEqual(req.headers.host, 'foo');
      strictEqual(req.headers['www-authenticate'], 'foo, bar, baz');
      strictEqual(req.headers['proxy-authenticate'], 'foo, bar, baz');
      strictEqual(req.headers['x-foo'], 'bingo');
      strictEqual(req.headers['x-bar'], 'banjo, bango');
      strictEqual(req.headers['sec-websocket-protocol'], 'chat, share');
      strictEqual(
        req.headers['sec-websocket-extensions'],
        'foo; 1, bar; 2, baz'
      );
      strictEqual(req.headers.constructor, 'foo, bar, baz');

      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.end('EOF');

      server.close();
    });

    server.listen(8080, async function () {
      const res = await env.SERVICE.fetch('https://cloudflare.com', {
        headers: [
          ['accept', 'abc'],
          ['accept', 'def'],
          ['Accept', 'ghijklmnopqrst'],
          ['host', 'foo'],
          ['Host', 'bar'],
          ['hOst', 'baz'],
          ['www-authenticate', 'foo'],
          ['WWW-Authenticate', 'bar'],
          ['WWW-AUTHENTICATE', 'baz'],
          ['proxy-authenticate', 'foo'],
          ['Proxy-Authenticate', 'bar'],
          ['PROXY-AUTHENTICATE', 'baz'],
          ['x-foo', 'bingo'],
          ['x-bar', 'banjo'],
          ['x-bar', 'bango'],
          ['sec-websocket-protocol', 'chat'],
          ['sec-websocket-protocol', 'share'],
          ['sec-websocket-extensions', 'foo; 1'],
          ['sec-websocket-extensions', 'bar; 2'],
          ['sec-websocket-extensions', 'baz'],
          ['constructor', 'foo'],
          ['constructor', 'bar'],
          ['constructor', 'baz'],
        ],
      });

      strictEqual(res.status, 200);
      strictEqual(res.headers.get('content-type'), 'text/plain');
      strictEqual(await res.text(), 'EOF');

      resolve();
    });

    await promise;
  },
};

// Test is taken from test/parallel/test-http-server-multiheaders2.js
export const testHttpServerMultiHeaders2 = {
  async test(_ctrl, env) {
    // One difference between Node.js and Cloudflare workers is that Cookie is allowed
    // to have multiple values but in Workers it is not supported.
    const multipleAllowed = [
      'Accept',
      'Accept-Charset',
      'Accept-Encoding',
      'Accept-Language',
      'Connection',
      'DAV', // GH-2750
      'Pragma', // GH-715
      'Link', // GH-1187
      'WWW-Authenticate', // GH-1083
      'Proxy-Authenticate', // GH-4052
      'Sec-Websocket-Extensions', // GH-2764
      'Sec-Websocket-Protocol', // GH-2764
      'Via', // GH-6660

      // not a special case, just making sure it's parsed correctly
      'X-Forwarded-For',

      // Make sure that unspecified headers is treated as multiple
      'Some-Random-Header',
      'X-Some-Random-Header',
    ];

    const multipleForbidden = [
      'Content-Type',
      'User-Agent',
      'Referer',
      'Host',
      'Authorization',
      'Proxy-Authorization',
      'If-Modified-Since',
      'If-Unmodified-Since',
      'From',
      'Location',
      'Max-Forwards',
    ];

    await using server = http.createServer(function (req, res) {
      for (const header of multipleForbidden) {
        const value = req.headers[header.toLowerCase()];
        strictEqual(
          value,
          'foo',
          `multiple forbidden header parsed incorrectly: ${header} with value: "${value}"`
        );
      }
      for (const header of multipleAllowed) {
        const sep = header.toLowerCase() === 'cookie' ? '; ' : ', ';
        strictEqual(
          req.headers[header.toLowerCase()],
          `foo${sep}bar`,
          `multiple allowed header parsed incorrectly: ${header}`
        );
      }

      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.end('EOF');
    });

    function makeHeader(value) {
      return function (header) {
        return [header, value];
      };
    }

    const headers = []
      .concat(multipleAllowed.map(makeHeader('foo')))
      .concat(multipleForbidden.map(makeHeader('foo')))
      .concat(multipleAllowed.map(makeHeader('bar')))
      .concat(multipleForbidden.map(makeHeader('bar')));

    server.listen(8080);

    const res = await env.SERVICE.fetch('https://cloudflare.com/', {
      headers,
    });
    strictEqual(res.status, 200);
  },
};

// Test for RFC 7230 compliant header splitting with quoted strings and escaped characters
export const testHttpServerQuotedStringHeaders = {
  async test(_ctrl, env) {
    await using server = http.createServer((req, res) => {
      // Basic quoted strings with commas
      strictEqual(req.headers['content-type'], 'text/plain; f="a, b, c"');
      strictEqual(req.headers['authorization'], 'Bearer token="abc, def"');
      strictEqual(
        req.headers['proxy-authorization'],
        'Basic realm="test, realm"'
      );

      // Escaped characters in quoted strings
      strictEqual(
        req.headers['user-agent'],
        'Mozilla/5.0; comment="has \\"quotes\\" and, commas"'
      );

      res.writeHead(200, { 'content-type': 'text/plain' });
      res.end('ok');
    });

    server.listen(8080);
    const res = await env.SERVICE.fetch('https://cloudflare.com', {
      method: 'GET',
      headers: [
        // Basic quoted string tests
        ['content-type', 'text/plain; f="a, b, c"'],
        ['content-type', 'text/foo; a="1, 2, 3"'],
        ['authorization', 'Bearer token="abc, def"'],
        ['authorization', 'Bearer token="ghi, jkl"'],
        ['proxy-authorization', 'Basic realm="test, realm"'],
        ['proxy-authorization', 'Basic realm="another, realm"'],
        // Escaped character tests
        ['user-agent', 'Mozilla/5.0; comment="has \\"quotes\\" and, commas"'],
        ['user-agent', 'Chrome/100.0; info="version \\"100\\""'],
      ],
    });

    strictEqual(res.status, 200);
    strictEqual(await res.text(), 'ok');
  },
};

// Test is taken from test/parallel/test-http-server-non-utf8-header.js
export const testHttpServerNonUtf8Header = {
  async test(_ctrl, env) {
    const nonUtf8Header = 'bår';
    const nonUtf8ToLatin1 = Buffer.from(nonUtf8Header).toString('latin1');

    {
      await using server = http.createServer((req, res) => {
        res.writeHead(200, [
          'content-disposition',
          Buffer.from(nonUtf8Header).toString('binary'),
        ]);
        res.end('hello');
      });

      server.listen(8080);
      const res = await env.SERVICE.fetch('https://cloudflare.com', {
        method: 'GET',
      });
      strictEqual(res.status, 200);
      strictEqual(res.headers.get('content-disposition'), nonUtf8ToLatin1);
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      // Test multi-value header
      await using server = http.createServer((req, res) => {
        res.writeHead(200, [
          'content-disposition',
          [Buffer.from(nonUtf8Header).toString('binary')],
        ]);
        res.end('hello');
      });

      server.listen(8080);
      const res = await env.SERVICE.fetch('https://cloudflare.com');
      strictEqual(res.status, 200);
      strictEqual(res.headers.get('content-disposition'), nonUtf8ToLatin1);
    }

    {
      await using server = http.createServer((req, res) => {
        res.writeHead(200, [
          'Content-Length',
          '5',
          'content-disposition',
          Buffer.from(nonUtf8Header).toString('binary'),
        ]);
        res.end('hello');
      });

      server.listen(8080);

      const res = await env.SERVICE.fetch('https://cloudflare.com');
      strictEqual(res.status, 200);
      // The issue is that Content-Length causes different header encoding behavior
      // We expect the raw bytes to be interpreted as UTF-8 by the fetch API
      strictEqual(res.headers.get('content-disposition'), nonUtf8Header);
    }
  },
};

// Test is taken from test/parallel/test-http-server-options-incoming-message.js
export const testHttpServerOptionsIncomingMessage = {
  async test(_ctrl, env) {
    class MyIncomingMessage extends http.IncomingMessage {
      getUserAgent() {
        return this.headers['user-agent'] || 'unknown';
      }
    }

    await using server = http.createServer(
      {
        IncomingMessage: MyIncomingMessage,
      },
      (req, res) => {
        strictEqual(req.getUserAgent(), 'node-test');
        res.statusCode = 200;
        res.end();
      }
    );
    server.listen(8080);

    const res = await env.SERVICE.fetch('https://cloudflare.com', {
      headers: { 'User-Agent': 'node-test' },
    });
    strictEqual(res.status, 200);
  },
};

// Test is taken from test/parallel/test-http-server-options-server-response.js
export const testHttpServerOptionsServerResponse = {
  async test(_ctrl, env) {
    class MyServerResponse extends http.ServerResponse {
      status(code) {
        return this.writeHead(code, { 'Content-Type': 'text/plain' });
      }
    }

    await using server = http.createServer(
      {
        ServerResponse: MyServerResponse,
      },
      (_req, res) => {
        res.status(200);
        res.end();
      }
    );
    server.listen(8080);

    const res = await env.SERVICE.fetch('https://cloudflare.com');
    strictEqual(res.status, 200);
    strictEqual(res.headers.get('content-type'), 'text/plain');
  },
};

// Test is taken from test/parallel/test-http-server-timeouts-validation.js
export const testHttpServerTimeoutsValidation = {
  async test() {
    // This test validates that the HTTP server timeouts are properly validated and set.

    {
      const server = http.createServer();
      strictEqual(server.headersTimeout, 60000);
      strictEqual(server.requestTimeout, 300000);
    }

    {
      const server = http.createServer({
        headersTimeout: 10000,
        requestTimeout: 20000,
      });
      strictEqual(server.headersTimeout, 10000);
      strictEqual(server.requestTimeout, 20000);
    }

    {
      const server = http.createServer({
        headersTimeout: 10000,
        requestTimeout: 10000,
      });
      strictEqual(server.headersTimeout, 10000);
      strictEqual(server.requestTimeout, 10000);
    }

    {
      const server = http.createServer({ headersTimeout: 10000 });
      strictEqual(server.headersTimeout, 10000);
      strictEqual(server.requestTimeout, 300000);
    }

    {
      const server = http.createServer({ requestTimeout: 20000 });
      strictEqual(server.headersTimeout, 20000);
      strictEqual(server.requestTimeout, 20000);
    }

    {
      const server = http.createServer({ requestTimeout: 100000 });
      strictEqual(server.headersTimeout, 60000);
      strictEqual(server.requestTimeout, 100000);
    }

    {
      throws(
        () =>
          http.createServer({ headersTimeout: 10000, requestTimeout: 1000 }),
        { code: 'ERR_OUT_OF_RANGE' }
      );
    }
  },
};

// Test is taken from test/parallel/test-http-server-write-after-end.js
export const testHttpServerWriteAfterEnd = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    await using server = http.createServer(handle);

    function handle(_req, res) {
      res.write('hello');
      res.end();

      queueMicrotask(() => {
        res.write('world', (err) => {
          strictEqual(err.code, 'ERR_STREAM_WRITE_AFTER_END');
          resolve();
        });
      });
    }

    server.listen(8080);
    await env.SERVICE.fetch('https://cloudflare.com');
    await promise;
  },
};

// Test is taken from test/parallel/test-http-server-write-end-after-end.js
export const testHttpServerWriteEndAfterEnd = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    const handle = mock.fn((req, res) => {
      res.write('hello');
      res.end();

      queueMicrotask(() => {
        res.end('world');
        res.write('world', (err) => {
          strictEqual(err.code, 'ERR_STREAM_WRITE_AFTER_END');
          resolve();
        });
      });
    });
    const server = http.createServer(handle);
    server.listen(8080);

    await env.SERVICE.fetch('https://cloudflare.com');
    await promise;
    strictEqual(handle.mock.callCount(), 1);
    server.close();
  },
};

export const testHandleZeroNullUndefinedPortNumber = {
  async test() {
    // Test zero port number.
    {
      const { promise, resolve } = Promise.withResolvers();
      const server = http.createServer();
      const listeningFn = mock.fn();
      server.on('listening', listeningFn);
      server.listen(0, () => {
        ok(server.listening);
        notStrictEqual(server.address().port, 0);
        strictEqual(listeningFn.mock.callCount(), 1);
        server.close();
        resolve();
      });
      await promise;
    }
    // Test null/undefined port number.
    {
      const { promise, resolve } = Promise.withResolvers();
      const server = http.createServer();
      const listeningFn = mock.fn();
      server.on('listening', listeningFn);
      server.listen(() => {
        ok(server.listening);
        notStrictEqual(server.address().port, 0);
        strictEqual(listeningFn.mock.callCount(), 1);
        server.close();
        resolve();
      });
      await promise;
    }
  },
};

export const testInvalidPorts = {
  async test() {
    const server = http.createServer();
    for (const value of [NaN, Infinity, -1, 1.1, 9999999]) {
      throws(() => server.listen(value), {
        code: 'ERR_SOCKET_BAD_PORT',
      });
    }
    strictEqual(server.listening, false);
  },
};

export const consumeRequestPayloadData = {
  async test(_ctrl, env) {
    await using server = http.createServer((req, res) => {
      const path = req.url;

      if (path === '/small') {
        strictEqual(req.method, 'POST');
        let data = '';
        req.setEncoding('utf8');
        req.on('data', (d) => (data += d));
        req.on('end', () => {
          strictEqual(data, 'hello world');
          res.setHeaders(new Headers({ hello: 'world' }));
          res.end(data + ' x2');
        });
      } else if (path === '/large-streaming') {
        strictEqual(req.method, 'POST');
        const dataEvents = [];
        let totalBytes = 0;

        req.on('data', (chunk) => {
          dataEvents.push({
            size: chunk.length,
            firstByte: chunk[0],
            lastByte: chunk[chunk.length - 1],
          });
          totalBytes += chunk.length;
        });

        req.on('end', () => {
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(
            JSON.stringify({
              totalEvents: dataEvents.length,
              totalBytes,
              firstEventSize: dataEvents[0]?.size,
              lastEventSize: dataEvents[dataEvents.length - 1]?.size,
              allSamePattern: dataEvents.every(
                (e) => e.firstByte === e.lastByte
              ),
            })
          );
        });
      }
    });

    server.listen(8080);

    {
      const res = await env.SERVICE.fetch('https://cloudflare.com/small', {
        body: 'hello world',
        method: 'POST',
      });
      strictEqual(res.status, 200);
      strictEqual(await res.text(), 'hello world x2');
      strictEqual(res.headers.get('hello'), 'world');
    }

    {
      const largeData = Buffer.alloc(256 * 1024, 123); // 256KB of byte value 123
      const res = await env.SERVICE.fetch(
        'https://cloudflare.com/large-streaming',
        {
          body: largeData,
          method: 'POST',
        }
      );
      strictEqual(res.status, 200);

      const result = JSON.parse(await res.text());
      ok(
        result.totalEvents > 1,
        `Should have multiple data events, got ${result.totalEvents}`
      );
      strictEqual(result.totalBytes, 256 * 1024);
      ok(
        result.allSamePattern,
        'All chunks should contain the same byte pattern'
      );
      ok(
        result.firstEventSize > 0 && result.lastEventSize > 0,
        'Events should have positive sizes'
      );
    }
  },
};

// Test large streaming responses and various data types
export const testStreamingResponses = {
  async test(_ctrl, env) {
    await using server = http.createServer((req, res) => {
      const path = req.url;

      if (path === '/large') {
        // Test 1: Large payload streaming
        const CHUNK_SIZE = 1024 * 64; // 64KB
        const NUM_CHUNKS = 10; // 640KB total
        res.writeHead(200, { 'X-Test': 'large' });
        for (let i = 0; i < NUM_CHUNKS; i++) {
          res.write(Buffer.alloc(CHUNK_SIZE, i % 256));
        }
        res.end();
      } else if (path === '/echo') {
        // Test 2: Echo with backpressure
        res.writeHead(200);
        req.pipe(res);
      } else if (path === '/mixed') {
        // Test 3: Mixed data types and implicit headers
        res.setHeader('X-Custom', 'mixed');
        res.write('string|');
        res.write(Buffer.from('buffer|'));
        res.write(new Uint8Array([85, 56, 124])); // "U8|"
        res.write(''); // Empty
        res.write('utf8-ñ|', 'utf8');
        setTimeout(() => res.end('async'), 10);
      } else if (path === '/many') {
        // Test 4: Many small writes
        res.writeHead(200);
        for (let i = 0; i < 100; i++) {
          res.write(`${i}|`);
        }
        res.end('END');
      }
    });

    server.listen(8080);

    // Test 1: Large payload
    const res1 = await env.SERVICE.fetch('https://cloudflare.com/large');
    strictEqual(res1.status, 200);
    const data1 = await res1.arrayBuffer();
    strictEqual(data1.byteLength, 1024 * 64 * 10);

    // Test 2: Echo
    const testData = Buffer.alloc(1024 * 128, 42);
    const res2 = await env.SERVICE.fetch('https://cloudflare.com/echo', {
      method: 'POST',
      body: testData,
    });
    const echo = Buffer.from(await res2.arrayBuffer());
    ok(echo.equals(testData));

    // Test 3: Mixed data types
    const res3 = await env.SERVICE.fetch('https://cloudflare.com/mixed');
    strictEqual(res3.status, 200);
    strictEqual(res3.headers.get('X-Custom'), 'mixed');
    strictEqual(await res3.text(), 'string|buffer|U8|utf8-ñ|async');

    // Test 4: Many writes
    const res4 = await env.SERVICE.fetch('https://cloudflare.com/many');
    const text4 = await res4.text();
    ok(text4.endsWith('98|99|END'));
  },
};

// Test Content-Length enforcement
export const testContentLengthEnforcement = {
  async test(_ctrl, env) {
    await using server = http.createServer((req, res) => {
      const path = req.url;

      if (path === '/too-few') {
        res.writeHead(200, { 'Content-Length': '20' });
        res.write('0123456789'); // Only 10 bytes
        res.end();
      } else if (path === '/too-many') {
        res.writeHead(200, { 'Content-Length': '10' });
        res.write('0123456789');
        res.write('0123456789'); // 20 bytes total
        res.end();
      } else if (path === '/exact') {
        res.writeHead(200, { 'Content-Length': '15' });
        res.write('Hello ');
        res.write('World!!!');
        res.end('!');
      }
    });

    server.listen(8080);

    // Test 1: Too few bytes
    {
      const res = await env.SERVICE.fetch('https://cloudflare.com/too-few');
      strictEqual(res.status, 200);
      strictEqual(await res.text(), '0123456789');
    }

    // Test 2: Too many bytes
    {
      const res = await env.SERVICE.fetch('https://cloudflare.com/too-many');
      strictEqual(res.status, 200);
      strictEqual(await res.text(), '0123456789');
    }

    // Test 3: Exact match
    {
      const res = await env.SERVICE.fetch('https://cloudflare.com/exact');
      strictEqual(res.status, 200);
      strictEqual(await res.text(), 'Hello World!!!!');
    }
  },
};

export const testCorkUncorkBasic = {
  async test(_ctrl, env) {
    await using server = http.createServer((req, res) => {
      res.writeHead(200, { 'Content-Type': 'text/plain' });

      strictEqual(res.writableLength, 0);
      res.cork();
      strictEqual(res.writableLength, 0);
      res.write('chunk1');
      strictEqual(res.writableLength, 108);
      res.write('chunk2');
      strictEqual(res.writableLength, 114);
      res.write('chunk3');
      strictEqual(res.writableLength, 120);
      res.uncork();
      strictEqual(res.writableLength, 0);

      res.end('final');
    });

    server.listen(8080);

    const res = await env.SERVICE.fetch('https://cloudflare.com');
    strictEqual(res.status, 200);
    strictEqual(await res.text(), 'chunk1chunk2chunk3final');
  },
};

export const testBackpressureSignaling = {
  async test(_ctrl, env) {
    const events = [];

    await using server = http.createServer((req, res) => {
      res.writeHead(200, { 'Content-Type': 'application/octet-stream' });

      let writeCount = 0;
      let drainCount = 0;

      res.on('drain', () => {
        drainCount++;
        events.push({ type: 'drain', count: drainCount });
        continueWriting();
      });

      const continueWriting = () => {
        while (writeCount < 50) {
          const chunk = Buffer.alloc(1024 * 32, writeCount % 256);
          const canContinue = res.write(chunk);

          strictEqual(typeof canContinue, 'boolean');
          events.push({
            type: 'write',
            canContinue,
            writeCount,
            writableLength: res.writableLength,
          });

          writeCount++;

          if (!canContinue) {
            events.push({ type: 'backpressure', writeCount });
            return;
          }
        }

        res.end();
      };

      continueWriting();
    });

    server.listen(8080);

    const res = await env.SERVICE.fetch('https://cloudflare.com');
    strictEqual(res.status, 200);
    strictEqual((await res.arrayBuffer()).byteLength, 1638400);

    const writeEvents = events.filter((e) => e.type === 'write');
    const backpressureEvents = events.filter((e) => e.type === 'backpressure');
    const drainEvents = events.filter((e) => e.type === 'drain');

    strictEqual(writeEvents.length, 50);

    writeEvents.forEach((e) => {
      strictEqual(typeof e.canContinue, 'boolean');
      strictEqual(typeof e.writableLength, 'number');
      ok(e.writableLength >= 0);
    });

    if (backpressureEvents.length > 0) {
      ok(
        drainEvents.length > 0,
        'Should emit drain events when backpressure occurs'
      );

      for (let i = 0; i < backpressureEvents.length; i++) {
        ok(
          drainEvents[i],
          `Should have corresponding drain event for backpressure ${i}`
        );
      }
    }
  },
};

// Test is taken from test/parallel/test-http-server.js
export const testHttpServer = {
  async test(_ctrl, env) {
    const invalid_options = ['foo', 42, true, []];

    for (const option of invalid_options) {
      throws(
        () => {
          new http.Server(option);
        },
        {
          code: 'ERR_INVALID_ARG_TYPE',
        }
      );
    }

    let request_number = 0;

    await using server = http.createServer(function (req, res) {
      res.id = request_number;
      req.id = request_number++;

      strictEqual(res.req, req);

      if (req.id === 0) {
        strictEqual(req.method, 'GET');
        strictEqual(url.parse(req.url).pathname, '/hello');
        strictEqual(qs.parse(url.parse(req.url).query).hello, 'world');
        strictEqual(qs.parse(url.parse(req.url).query).foo, 'b==ar');
      }

      if (req.id === 1) {
        strictEqual(req.method, 'POST');
        strictEqual(url.parse(req.url).pathname, '/quit');
      }

      if (req.id === 2) {
        strictEqual(req.headers['x-x'], 'foo');
      }

      if (req.id === 3) {
        strictEqual(req.headers['x-x'], 'bar');
        this.close();
      }

      setTimeout(function () {
        res.writeHead(200, { 'Content-Type': 'text/plain' });
        res.write(url.parse(req.url).pathname);
        res.end();
      }, 1);
    });
    server.listen(8080);

    server.httpAllowHalfOpen = true;

    const hello = await env.SERVICE.fetch(
      'https://example.com/hello?hello=world&foo=b==ar'
    );
    strictEqual(hello.status, 200);
    strictEqual(await hello.text(), '/hello');

    const quit = await env.SERVICE.fetch('https://example.com/quit', {
      method: 'POST',
    });
    strictEqual(quit.status, 200);
    strictEqual(await quit.text(), '/quit');

    const xxFoo = await env.SERVICE.fetch('https://example.com/', {
      method: 'POST',
      headers: {
        'x-x': 'foo',
      },
    });
    strictEqual(xxFoo.status, 200);
    strictEqual(await xxFoo.text(), '/');

    const xxBar = await env.SERVICE.fetch('https://example.com/', {
      method: 'POST',
      headers: {
        'x-x': 'bar',
      },
    });
    strictEqual(xxBar.status, 200);
    strictEqual(await xxBar.text(), '/');

    strictEqual(request_number, 4);
  },
};

// Test multiple pipe destinations (Node.js feature that web streams don't support)
export const testMultiplePipeDestinations = {
  async test(_ctrl, env) {
    const { Writable } = await import('node:stream');

    await using server = http.createServer((req, res) => {
      const path = req.url;

      if (path === '/multipipe') {
        res.writeHead(200, { 'Content-Type': 'application/json' });

        // Create multiple writable destinations
        const dest1Data = [];
        const dest2Data = [];
        const dest3Data = [];

        const dest1 = new Writable({
          write(chunk, encoding, callback) {
            dest1Data.push(chunk);
            callback();
          },
        });

        const dest2 = new Writable({
          write(chunk, encoding, callback) {
            dest2Data.push(chunk);
            callback();
          },
        });

        const dest3 = new Writable({
          write(chunk, encoding, callback) {
            dest3Data.push(chunk);
            callback();
          },
        });

        // Set up finish handlers to track completion
        let finishedCount = 0;
        const onFinish = () => {
          finishedCount++;
          if (finishedCount === 3) {
            // All destinations finished, send response
            const result = {
              dest1: Buffer.concat(dest1Data).toString(),
              dest2: Buffer.concat(dest2Data).toString(),
              dest3: Buffer.concat(dest3Data).toString(),
              allSame:
                Buffer.concat(dest1Data).equals(Buffer.concat(dest2Data)) &&
                Buffer.concat(dest2Data).equals(Buffer.concat(dest3Data)),
            };
            res.end(JSON.stringify(result));
          }
        };

        dest1.on('finish', onFinish);
        dest2.on('finish', onFinish);
        dest3.on('finish', onFinish);

        // Pipe to multiple destinations - this is the key test!
        req.pipe(dest1);
        req.pipe(dest2);
        req.pipe(dest3);
      } else {
        res.writeHead(404);
        res.end('Not Found');
      }
    });

    server.listen(8080);

    // Send test data
    const testData =
      'Hello from multiple pipes! This data should reach all destinations.';
    const response = await env.SERVICE.fetch(
      'https://cloudflare.com/multipipe',
      {
        method: 'POST',
        body: testData,
        headers: {
          'Content-Type': 'text/plain',
        },
      }
    );

    strictEqual(response.status, 200);
    const result = await response.json();

    // Verify all destinations received the same data
    strictEqual(
      result.dest1,
      testData,
      'Destination 1 should receive correct data'
    );
    strictEqual(
      result.dest2,
      testData,
      'Destination 2 should receive correct data'
    );
    strictEqual(
      result.dest3,
      testData,
      'Destination 3 should receive correct data'
    );
    strictEqual(
      result.allSame,
      true,
      'All destinations should receive identical data'
    );
  },
};

export const testScheduled = {
  async test(_ctrl, env) {
    strictEqual(typeof env.SERVICE.scheduled, 'function');

    await env.SERVICE.scheduled({
      scheduledTime: Date.now(),
      cron: '0 0 * * *',
    });

    strictEqual(scheduledCallCount, 1);
  },
};

export const testConfigurableHighWaterMark = {
  async test(_ctrl, env) {
    {
      await using server = http.createServer({ highWaterMark: 9999 });
      strictEqual(server.highWaterMark, 9999);
    }

    {
      // Node.js supports 1.1 as a value for highWaterMark
      await using server = http.createServer({ highWaterMark: 1.11 });
      strictEqual(server.highWaterMark, 1.11);
    }

    {
      // Node.js omits negative values
      await using server = http.createServer({ highWaterMark: -1 });
      strictEqual(server.highWaterMark, 65536);
    }

    for (const highWaterMark of [null, 'hello world', ['merhaba dunya']]) {
      throws(() => http.createServer({ highWaterMark }), {
        code: 'ERR_INVALID_ARG_TYPE',
      });
    }
  },
};

export const testIncomingMessageSocket = {
  async test(_ctrl, env) {
    await using server = http.createServer((req, res) => {
      strictEqual(req.socket.encrypted, false);
      strictEqual(req.socket.localPort, 8080);
      strictEqual(req.socket.localAddress, '127.0.0.1');
      strictEqual(req.socket.remoteAddress, '127.0.0.1');
      strictEqual(typeof req.socket.remotePort, 'number');
      ok(req.socket.remotePort >= Math.pow(2, 15));
      ok(req.socket.remotePort <= Math.pow(2, 16));
      strictEqual(req.socket.remoteFamily, 'IPv4');

      res.writeHead(200);
      res.end('Hello, World!');
    });

    server.listen(8080);

    const res = await env.SERVICE.fetch('https://cloudflare.com');
    strictEqual(res.status, 200);
    strictEqual(await res.text(), 'Hello, World!');
  },
};

let scheduledCallCount = 0;

export default {
  fetch(request) {
    return handleAsNodeRequest(8080, request);
  },
  async scheduled(event) {
    scheduledCallCount++;
    strictEqual(typeof event.scheduledTime, 'number');
    strictEqual(typeof event.cron, 'string');
  },
};

// Relevant Node.js tests
// - [x] test/parallel/test-http-server-async-dispose.js
// - [ ] test/parallel/test-http-server-capture-rejections.js
// - [ ] test/parallel/test-http-server-close-idle-wait-response.js
// - [ ] test/parallel/test-http-server-close-idle.js
// - [ ] test/parallel/test-http-server-consumed-timeout.js
// - [ ] test/parallel/test-http-server-headers-timeout-delayed-headers.js
// - [ ] test/parallel/test-http-server-headers-timeout-interrupted-headers.js
// - [ ] test/parallel/test-http-server-headers-timeout-keepalive.js
// - [ ] test/parallel/test-http-server-headers-timeout-pipelining.js
// - [x] test/parallel/test-http-server-incomingmessage-destroy.js
// - [x] test/parallel/test-http-server-method.query.js
// - [x] test/parallel/test-http-server-multiheaders.js
// - [x] test/parallel/test-http-server-multiheaders2.js
// - [x] test/parallel/test-http-server-non-utf8-header.js
// - [x] test/parallel/test-http-server-options-incoming-message.js
// - [x] test/parallel/test-http-server-options-server-response.js
// - [ ] test/parallel/test-http-server-request-timeout-delayed-body.js
// - [ ] test/parallel/test-http-server-request-timeout-delayed-headers.js
// - [ ] test/parallel/test-http-server-request-timeout-interrupted-body.js
// - [ ] test/parallel/test-http-server-request-timeout-interrupted-headers.js
// - [ ] test/parallel/test-http-server-request-timeout-keepalive.js
// - [ ] test/parallel/test-http-server-request-timeout-pipelining.js
// - [ ] test/parallel/test-http-server-request-timeout-upgrade.js
// - [x] test/parallel/test-http-server-timeouts-validation.js
// - [x] test/parallel/test-http-server-write-after-end.js
// - [x] test/parallel/test-http-server-write-end-after-end.js
// - [x] test/parallel/test-http-server.js

// Tests that does not apply to workerd.
// - [ ] test/parallel/test-http-server-connection-list-when-close.js
// - [ ] test/parallel/test-http-server-connections-checking-leak.js
// - [ ] test/parallel/test-http-server-clear-timer.js
// - [ ] test/parallel/test-http-server-client-error.js
// - [ ] test/parallel/test-http-server-close-all.js
// - [ ] test/parallel/test-http-server-close-destroy-timeout.js
// - [ ] test/parallel/test-http-server-de-chunked-trailer.js
// - [ ] test/parallel/test-http-server-delete-parser.js
// - [ ] test/parallel/test-http-server-destroy-socket-on-client-error.js
// - [ ] test/parallel/test-http-server-keep-alive-defaults.js
// - [ ] test/parallel/test-http-server-keep-alive-max-requests-null.js
// - [ ] test/parallel/test-http-server-keep-alive-timeout.js
// - [ ] test/parallel/test-http-server-keepalive-end.js
// - [ ] test/parallel/test-http-server-keepalive-req-gc.js
// - [ ] test/parallel/test-http-server-options-highwatermark.js
// - [ ] test/parallel/test-http-server-multiple-client-error.js
// - [ ] test/parallel/test-http-server-reject-chunked-with-content-length.js
// - [ ] test/parallel/test-http-server-reject-cr-no-lf.js
// - [ ] test/parallel/test-http-server-response-standalone.js
// - [ ] test/parallel/test-http-server-stale-close.js
// - [ ] test/parallel/test-http-server-unconsume.js
// - [ ] test/parallel/test-http-server-unconsume-consume.js
