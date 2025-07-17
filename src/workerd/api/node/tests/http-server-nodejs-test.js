import http from 'node:http';
import { strictEqual, ok } from 'node:assert';
import { registerFetchEvents } from 'cloudflare:workers';

export const checkPortsSetCorrectly = {
  test(_ctrl, env) {
    const keys = ['PONG_SERVER_PORT'];
    for (const key of keys) {
      strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
  },
};

export const testHttpServerMultiHeaders = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    const server = http.createServer(function (req, res) {
      strictEqual(req.headers.accept, 'abc, def, ghijklmnopqrst');
      // TODO(soon): host should not be joined.
      // strictEqual(req.headers.host, 'foo');
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

export default registerFetchEvents({ port: 8080 });

// Relevant Node.js tests
// - [ ] test/parallel/test-http-server-async-dispose.js
// - [ ] test/parallel/test-http-server-capture-rejections.js
// - [ ] test/parallel/test-http-server-clear-timer.js
// - [ ] test/parallel/test-http-server-client-error.js
// - [ ] test/parallel/test-http-server-close-all.js
// - [ ] test/parallel/test-http-server-close-destroy-timeout.js
// - [ ] test/parallel/test-http-server-close-idle-wait-response.js
// - [ ] test/parallel/test-http-server-close-idle.js
// - [ ] test/parallel/test-http-server-connection-list-when-close.js
// - [ ] test/parallel/test-http-server-connections-checking-leak.js
// - [ ] test/parallel/test-http-server-consumed-timeout.js
// - [ ] test/parallel/test-http-server-de-chunked-trailer.js
// - [ ] test/parallel/test-http-server-delete-parser.js
// - [ ] test/parallel/test-http-server-destroy-socket-on-client-error.js
// - [ ] test/parallel/test-http-server-headers-timeout-delayed-headers.js
// - [ ] test/parallel/test-http-server-headers-timeout-interrupted-headers.js
// - [ ] test/parallel/test-http-server-headers-timeout-keepalive.js
// - [ ] test/parallel/test-http-server-headers-timeout-pipelining.js
// - [ ] test/parallel/test-http-server-incomingmessage-destroy.js
// - [ ] test/parallel/test-http-server-keep-alive-defaults.js
// - [ ] test/parallel/test-http-server-keep-alive-max-requests-null.js
// - [ ] test/parallel/test-http-server-keep-alive-timeout.js
// - [ ] test/parallel/test-http-server-keepalive-end.js
// - [ ] test/parallel/test-http-server-keepalive-req-gc.js
// - [ ] test/parallel/test-http-server-method.query.js
// - [x] test/parallel/test-http-server-multiheaders.js
// - [ ] test/parallel/test-http-server-multiheaders2.js
// - [ ] test/parallel/test-http-server-multiple-client-error.js
// - [ ] test/parallel/test-http-server-non-utf8-header.js
// - [ ] test/parallel/test-http-server-options-highwatermark.js
// - [ ] test/parallel/test-http-server-options-incoming-message.js
// - [ ] test/parallel/test-http-server-options-server-response.js
// - [ ] test/parallel/test-http-server-reject-chunked-with-content-length.js
// - [ ] test/parallel/test-http-server-reject-cr-no-lf.js
// - [ ] test/parallel/test-http-server-request-timeout-delayed-body.js
// - [ ] test/parallel/test-http-server-request-timeout-delayed-headers.js
// - [ ] test/parallel/test-http-server-request-timeout-interrupted-body.js
// - [ ] test/parallel/test-http-server-request-timeout-interrupted-headers.js
// - [ ] test/parallel/test-http-server-request-timeout-keepalive.js
// - [ ] test/parallel/test-http-server-request-timeout-pipelining.js
// - [ ] test/parallel/test-http-server-request-timeout-upgrade.js
// - [ ] test/parallel/test-http-server-response-standalone.js
// - [ ] test/parallel/test-http-server-stale-close.js
// - [ ] test/parallel/test-http-server-timeouts-validation.js
// - [ ] test/parallel/test-http-server-unconsume-consume.js
// - [ ] test/parallel/test-http-server-unconsume.js
// - [ ] test/parallel/test-http-server-write-after-end.js
// - [ ] test/parallel/test-http-server-write-end-after-end.js
// - [ ] test/parallel/test-http-server.js
