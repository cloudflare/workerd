import http from 'node:http';
import { strictEqual, ok, throws, notStrictEqual, rejects } from 'node:assert';
import {
  nodeCompatHttpServerHandler,
  WorkerEntrypoint,
} from 'cloudflare:workers';
import { mock } from 'node:test';

export const checkPortsSetCorrectly = {
  test(_ctrl, env) {
    const keys = ['PONG_SERVER_PORT'];
    for (const key of keys) {
      strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
  },
};

export class GlobalService extends WorkerEntrypoint {}
Object.assign(
  GlobalService.prototype,
  nodeCompatHttpServerHandler({ port: 9090 })
);

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

    // TODO(soon): Investigate this. The test fails with the following assertion error.
    // It's probably caused by the difference in fetch() API usage.
    // -   bår
    // +   bÃ¥r
    // {
    //   await using server = http.createServer((req, res) => {
    //     res.writeHead(200, [
    //       'Content-Length',
    //       '5',
    //       'content-disposition',
    //       Buffer.from(nonUtf8Header).toString('binary'),
    //     ]);
    //     res.end('hello');
    //   });

    //   server.listen(8080);

    //   const res = await env.SERVICE.fetch('https://cloudflare.com');
    //   strictEqual(res.status, 200);
    //   strictEqual(res.headers.get('content-disposition'), nonUtf8ToLatin1);
    // }
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

      res.cork();
      res.write('chunk1');
      res.write('chunk2');
      res.write('chunk3');
      strictEqual(res.writableLength, 12);

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

export default nodeCompatHttpServerHandler({ port: 8080 });

// Relevant Node.js tests
// - [x] test/parallel/test-http-server-async-dispose.js
// - [ ] test/parallel/test-http-server-capture-rejections.js
// - [ ] test/parallel/test-http-server-client-error.js
// - [ ] test/parallel/test-http-server-close-destroy-timeout.js
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
// - [ ] test/parallel/test-http-server-multiheaders2.js
// - [ ] test/parallel/test-http-server-multiple-client-error.js
// - [x] test/parallel/test-http-server-non-utf8-header.js
// - [x] test/parallel/test-http-server-options-incoming-message.js
// - [x] test/parallel/test-http-server-options-server-response.js
// - [ ] test/parallel/test-http-server-reject-chunked-with-content-length.js
// - [ ] test/parallel/test-http-server-request-timeout-delayed-body.js
// - [ ] test/parallel/test-http-server-request-timeout-delayed-headers.js
// - [ ] test/parallel/test-http-server-request-timeout-interrupted-body.js
// - [ ] test/parallel/test-http-server-request-timeout-interrupted-headers.js
// - [ ] test/parallel/test-http-server-request-timeout-keepalive.js
// - [ ] test/parallel/test-http-server-request-timeout-pipelining.js
// - [ ] test/parallel/test-http-server-request-timeout-upgrade.js
// - [ ] test/parallel/test-http-server-timeouts-validation.js
// - [x] test/parallel/test-http-server-write-after-end.js
// - [x] test/parallel/test-http-server-write-end-after-end.js
// - [ ] test/parallel/test-http-server.js

// Tests that does not apply to workerd.
// - [ ] test/parallel/test-http-server-connection-list-when-close.js
// - [ ] test/parallel/test-http-server-connections-checking-leak.js
// - [ ] test/parallel/test-http-server-clear-timer.js
// - [ ] test/parallel/test-http-server-close-all.js
// - [ ] test/parallel/test-http-server-de-chunked-trailer.js
// - [ ] test/parallel/test-http-server-delete-parser.js
// - [ ] test/parallel/test-http-server-destroy-socket-on-client-error.js
// - [ ] test/parallel/test-http-server-keep-alive-defaults.js
// - [ ] test/parallel/test-http-server-keep-alive-max-requests-null.js
// - [ ] test/parallel/test-http-server-keep-alive-timeout.js
// - [ ] test/parallel/test-http-server-keepalive-end.js
// - [ ] test/parallel/test-http-server-keepalive-req-gc.js
// - [ ] test/parallel/test-http-server-options-highwatermark.js
// - [ ] test/parallel/test-http-server-reject-cr-no-lf.js
// - [ ] test/parallel/test-http-server-response-standalone.js
// - [ ] test/parallel/test-http-server-stale-close.js
// - [ ] test/parallel/test-http-server-unconsume.js
// - [ ] test/parallel/test-http-server-unconsume-consume.js
