import http from 'node:http';
import { strictEqual, ok, throws } from 'node:assert';
import { nodeCompatHttpServerHandler } from 'cloudflare:workers';
import { mock } from 'node:test';
import { Writable } from 'node:stream';

export const checkPortsSetCorrectly = {
  test(_ctrl, env) {
    const keys = ['PONG_SERVER_PORT'];
    for (const key of keys) {
      strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
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

// Test is taken from test/parallel/test-http-server-de-chunked-trailer.js
export const testHttpServerDeChunkedTrailer = {
  async test(_ctrl, env) {
    const handlerFn = mock.fn((req, res) => {
      res.setHeader('Trailer', 'baz');
      const trailerInvalidErr = {
        code: 'ERR_HTTP_TRAILER_INVALID',
        message: 'Trailers are invalid with this transfer encoding',
        name: 'Error',
      };
      throws(
        () => res.writeHead(200, { 'Content-Length': '2' }),
        trailerInvalidErr
      );
      res.removeHeader('Trailer');
      res.end('ok');
    });
    const server = http.createServer(handlerFn);

    server.listen(8080);

    const res = await env.SERVICE.fetch('https://cloudflare.com');
    strictEqual(res.status, 200);
    strictEqual(await res.text(), 'ok');
    strictEqual(handlerFn.mock.callCount(), 1);
    server.close();
  },
};

// TODO(soon): Fix this
// // Test is taken from test/parallel/test-http-server-incomingmessage-destroy.js
// export const testHttpServerIncomingMessageDestroy = {
//   async test(_ctrl, env) {
//     await using server = http.createServer((req, res) => {
//       req.destroy(new Error('Destroy test'));
//     });

//     server.listen(8080);

//     await rejects(() => {
//       return env.SERVICE.fetch('https://cloudflare.com');
//     }, {
//       message: '',
//     })
//   }
// }

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
      const { promise, resolve } = Promise.withResolvers();
      const server = http.createServer((req, res) => {
        res.writeHead(200, [
          'content-disposition',
          Buffer.from(nonUtf8Header).toString('binary'),
        ]);
        res.end('hello');
      });

      server.listen(8080, async () => {
        const res = await env.SERVICE.fetch('https://cloudflare.com', {
          method: 'GET',
        });
        strictEqual(res.status, 200);
        strictEqual(res.headers.get('content-disposition'), nonUtf8ToLatin1);
        server.close();
        resolve();
      });

      await promise;
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      // Test multi-value header
      const server = http.createServer((req, res) => {
        res.writeHead(200, [
          'content-disposition',
          [Buffer.from(nonUtf8Header).toString('binary')],
        ]);
        res.end('hello');
      });

      server.listen(8080, async () => {
        const res = await env.SERVICE.fetch('https://cloudflare.com');
        strictEqual(res.status, 200);
        strictEqual(res.headers.get('content-disposition'), nonUtf8ToLatin1);
        server.close();
        resolve();
      });
      await promise;
    }

    // TODO(soon): Investigate this.
    // {
    //   const { promise, resolve } = Promise.withResolvers();
    //   const server = http.createServer((req, res) => {
    //     res.writeHead(200, [
    //       'Content-Length',
    //       '5',
    //       'content-disposition',
    //       Buffer.from(nonUtf8Header).toString('binary'),
    //     ]);
    //     res.end('hello');
    //   });
    //
    //   server.listen(8080);
    //
    //   const res = await env.SERVICE.fetch('https://cloudflare.com');
    //   strictEqual(res.status, 200);
    //   strictEqual(res.headers.get('content-disposition'), nonUtf8ToLatin1);
    //   server.close();
    //   resolve();
    //
    //   await promise;
    // }
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

export default nodeCompatHttpServerHandler({ port: 8080 });

// Relevant Node.js tests
// - [x] test/parallel/test-http-server-async-dispose.js
// - [ ] test/parallel/test-http-server-capture-rejections.js
// - [ ] test/parallel/test-http-server-client-error.js
// - [ ] test/parallel/test-http-server-close-destroy-timeout.js
// - [ ] test/parallel/test-http-server-close-idle-wait-response.js
// - [ ] test/parallel/test-http-server-close-idle.js
// - [ ] test/parallel/test-http-server-connection-list-when-close.js
// - [ ] test/parallel/test-http-server-connections-checking-leak.js
// - [ ] test/parallel/test-http-server-consumed-timeout.js
// - [x] test/parallel/test-http-server-de-chunked-trailer.js
// - [ ] test/parallel/test-http-server-destroy-socket-on-client-error.js
// - [ ] test/parallel/test-http-server-headers-timeout-delayed-headers.js
// - [ ] test/parallel/test-http-server-headers-timeout-interrupted-headers.js
// - [ ] test/parallel/test-http-server-headers-timeout-keepalive.js
// - [ ] test/parallel/test-http-server-headers-timeout-pipelining.js
// - [x] test/parallel/test-http-server-incomingmessage-destroy.js
// - [ ] test/parallel/test-http-server-keep-alive-defaults.js
// - [ ] test/parallel/test-http-server-keep-alive-max-requests-null.js
// - [ ] test/parallel/test-http-server-keep-alive-timeout.js
// - [ ] test/parallel/test-http-server-keepalive-end.js
// - [ ] test/parallel/test-http-server-keepalive-req-gc.js
// - [ ] test/parallel/test-http-server-method.query.js
// - [x] test/parallel/test-http-server-multiheaders.js
// - [ ] test/parallel/test-http-server-multiheaders2.js
// - [ ] test/parallel/test-http-server-multiple-client-error.js
// - [x] test/parallel/test-http-server-non-utf8-header.js
// - [ ] test/parallel/test-http-server-options-incoming-message.js
// - [ ] test/parallel/test-http-server-options-server-response.js
// - [ ] test/parallel/test-http-server-reject-chunked-with-content-length.js
// - [ ] test/parallel/test-http-server-request-timeout-delayed-body.js
// - [ ] test/parallel/test-http-server-request-timeout-delayed-headers.js
// - [ ] test/parallel/test-http-server-request-timeout-interrupted-body.js
// - [ ] test/parallel/test-http-server-request-timeout-interrupted-headers.js
// - [ ] test/parallel/test-http-server-request-timeout-keepalive.js
// - [ ] test/parallel/test-http-server-request-timeout-pipelining.js
// - [ ] test/parallel/test-http-server-request-timeout-upgrade.js
// - [ ] test/parallel/test-http-server-stale-close.js
// - [ ] test/parallel/test-http-server-timeouts-validation.js
// - [ ] test/parallel/test-http-server-unconsume-consume.js
// - [ ] test/parallel/test-http-server-unconsume.js
// - [ ] test/parallel/test-http-server-write-after-end.js
// - [x] test/parallel/test-http-server-write-end-after-end.js
// - [ ] test/parallel/test-http-server.js

// Tests that does not apply to workerd.
// - [ ] test/parallel/test-http-server-clear-timer.js
// - [ ] test/parallel/test-http-server-close-all.js
// - [ ] test/parallel/test-http-server-delete-parser.js
// - [ ] test/parallel/test-http-server-options-highwatermark.js
// - [ ] test/parallel/test-http-server-reject-cr-no-lf.js
// - [ ] test/parallel/test-http-server-response-standalone.js
