import http from 'node:http';
import { throws, strictEqual, ok, deepStrictEqual } from 'node:assert';
import { httpServerHandler } from 'cloudflare:node';
import { mock } from 'node:test';
import stream from 'node:stream';

export const checkPortsSetCorrectly = {
  test(_ctrl, env) {
    const keys = [
      'SIDECAR_HOSTNAME',
      'FINISH_WRITABLE_PORT',
      'WRITABLE_FINISHED_PORT',
      'PROPERTIES_PORT',
    ];
    for (const key of keys) {
      strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
  },
};

// Test is taken from test/parallel/test-http-outgoing-destroy.js
export const testHttpOutgoingDestroy = {
  async test() {
    const { promise, resolve, reject } = Promise.withResolvers();
    const msg = new http.OutgoingMessage();
    strictEqual(msg.destroyed, false);
    msg.destroy();
    strictEqual(msg.destroyed, true);
    msg.write('asd', (err) => {
      strictEqual(err.code, 'ERR_STREAM_DESTROYED');
      resolve();
    });
    msg.on('error', reject);
    await promise;
  },
};

// Tests are taken from test/parallel/test-http-outgoing-finish-writable.js
export const testHttpOutgoingFinishWritable = {
  async test(_ctrl, env) {
    const clientRequest = http.request({
      hostname: env.SIDECAR_HOSTNAME,
      port: env.FINISH_WRITABLE_PORT,
      method: 'GET',
      path: '/',
    });

    strictEqual(clientRequest.writable, true);
    clientRequest.end();

    // Writable is still true when close
    // THIS IS LEGACY, we cannot change it
    // unless we break error detection
    strictEqual(clientRequest.writable, true);
  },
};

// Tests are taken from test/parallel/test-http-outgoing-properties.js
export const testHttpOutgoingProperties = {
  async test(_ctrl, env) {
    {
      const msg = new http.OutgoingMessage();
      msg._implicitHeader = function () {};
      strictEqual(msg.writableLength, 0);
      // TODO(soon): Implement _write() method of OutgoingMessage
      // msg.write('asd');
      // strictEqual(msg.writableLength, 3);
    }

    {
      const req = http.request({
        hostname: env.SIDECAR_HOSTNAME,
        port: env.PROPERTIES_PORT,
        method: 'GET',
        path: '/',
      });

      strictEqual(req.path, '/');
      strictEqual(req.method, 'GET');
      strictEqual(req.host, env.SIDECAR_HOSTNAME);
      strictEqual(req.protocol, 'http:');
      req.end();
    }
  },
};

// This is a modified test which is taken from
// https://github.com/nodejs/node/blob/462c74181d8e15e74bc5a25d55290d93bd7edf65/test/parallel/test-http-outgoing-proto.js#L47
export const testHttpOutgoingProto = {
  async test() {
    throws(
      () => {
        const outgoingMessage = new http.OutgoingMessage();
        outgoingMessage.setHeader();
      },
      {
        code: 'ERR_INVALID_HTTP_TOKEN',
        name: 'TypeError',
        message: 'Header name must be a valid HTTP token ["undefined"]',
      }
    );

    throws(
      () => {
        const outgoingMessage = new http.OutgoingMessage();
        outgoingMessage.setHeader('test');
      },
      {
        code: 'ERR_HTTP_INVALID_HEADER_VALUE',
        name: 'TypeError',
        message: 'Invalid value "undefined" for header "test"',
      }
    );

    throws(
      () => {
        const outgoingMessage = new http.OutgoingMessage();
        outgoingMessage.setHeader(404);
      },
      {
        code: 'ERR_INVALID_HTTP_TOKEN',
        name: 'TypeError',
        message: 'Header name must be a valid HTTP token ["404"]',
      }
    );

    throws(
      () => {
        const outgoingMessage = new http.OutgoingMessage();
        outgoingMessage.setHeader('200', 'あ');
      },
      {
        code: 'ERR_INVALID_CHAR',
        name: 'TypeError',
        message: 'Invalid character in header content ["200"]',
      }
    );

    {
      const outgoingMessage = new http.OutgoingMessage();
      strictEqual(outgoingMessage.destroyed, false);
      outgoingMessage.destroy();
      strictEqual(outgoingMessage.destroyed, true);
    }
  },
};

// Tests are taken from test/parallel/test-http-outgoing-renderHeaders.js
export const testHttpOutgoingRenderHeaders = {
  async test() {
    const outgoingMessage = new http.OutgoingMessage();
    outgoingMessage._header = {};
    throws(() => outgoingMessage._renderHeaders(), {
      code: 'ERR_HTTP_HEADERS_SENT',
      name: 'Error',
      message: 'Cannot render headers after they are sent to the client',
    });
  },
};

// Tests are taken from test/parallel/test-http-outgoing-writableFinished.js
export const testHttpOutgoingWritableFinished = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    const clientRequest = http.request({
      hostname: env.SIDECAR_HOSTNAME,
      port: env.WRITABLE_FINISHED_PORT,
      method: 'GET',
      path: '/',
    });

    strictEqual(clientRequest.writableFinished, false);
    clientRequest
      .on('finish', () => {
        strictEqual(clientRequest.writableFinished, true);
        resolve();
      })
      .end();
    strictEqual(clientRequest.writableFinished, false);
    await promise;
  },
};

// Test is taken from test/parallel/test-http-outgoing-destroyed.js
export const testHttpOutgoingDestroyed = {
  async test(_ctrl, env) {
    {
      const { promise, resolve } = Promise.withResolvers();
      const errorFn = mock.fn();
      await using server = http.createServer((req, res) => {
        strictEqual(res.closed, false);
        req.pipe(res);
        res.on('error', errorFn);
        res.on('close', () => {
          strictEqual(res.closed, true);
          res.end('asd');
          process.nextTick(resolve);
        });
      });

      server.listen(8080);

      await env.SERVICE.fetch('https://cloudflare.com', {
        method: 'PUT',
        body: 'asd',
      });
      await promise;
      strictEqual(errorFn.mock.callCount(), 0);
    }

    {
      await using server = http.createServer((req, res) => {
        strictEqual(res.closed, false);
        res.end();
        res.destroy();
        // Make sure not to emit 'error' after .destroy().
        res.end('asd');
        strictEqual(res.errored, undefined);
      });

      server.listen(8080);

      await env.SERVICE.fetch('https://cloudflare.com');
    }
  },
};

// Test is taken from test/parallel/test-http-outgoing-end-multiple.js
export const testHttpOutgoingEndMultiple = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    const onWriteAfterEndError = mock.fn((err) => {
      strictEqual(err.code, 'ERR_STREAM_WRITE_AFTER_END');
    });
    const resEndFn = mock.fn();
    await using server = http.createServer(function (req, res) {
      res.end('testing ended state', resEndFn);
      strictEqual(res.writableCorked, 0);
      res.end((err) => {
        strictEqual(err.code, 'ERR_STREAM_ALREADY_FINISHED');
      });
      strictEqual(res.writableCorked, 0);
      res.end('end', onWriteAfterEndError);
      strictEqual(res.writableCorked, 0);
      res.on('error', onWriteAfterEndError);
      res.on('finish', () => {
        res.end((err) => {
          strictEqual(err.code, 'ERR_STREAM_ALREADY_FINISHED');
          resolve();
        });
      });
    });

    server.listen(8080);

    await env.SERVICE.fetch('https://cloudflare.com/');
    await promise;

    strictEqual(onWriteAfterEndError.mock.callCount(), 2);
    strictEqual(resEndFn.mock.callCount(), 1);
  },
};

// Test is taken from test/parallel/test-http-outgoing-write-types.js
export const testHttpOutgoingWriteTypes = {
  async test(_ctrl, env) {
    await using httpServer = http.createServer(function (req, res) {
      throws(
        () => {
          res.write(['Throws.']);
        },
        { code: 'ERR_INVALID_ARG_TYPE' }
      );
      // should not throw
      res.write('1a2b3c');
      // should not throw
      res.write(new Uint8Array(1024));
      // should not throw
      res.write(Buffer.from('1'.repeat(1024)));
      res.end();
    });

    httpServer.listen(8080);

    await env.SERVICE.fetch('https://cloudflare.com');
  },
};

// Test is taken from test/parallel/test-http-outgoing-buffer.js
export const testHttpOutgoingBuffer = {
  async test() {
    const msg = new http.OutgoingMessage();
    msg._implicitHeader = function () {};

    // Writes should be buffered until highwatermark
    // even when no socket is assigned.

    strictEqual(msg.write('asd'), true);
    while (msg.write('asd'));
    const highwatermark = msg.writableHighWaterMark || 65535;
    ok(msg.outputSize >= highwatermark);
  },
};

// Test is taken from test/parallel/test-http-outgoing-finished.js
export const testHttpOutgoingFinished = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    await using server = http.createServer(function (req, res) {
      let closed = false;
      res
        .on('close', () => {
          closed = true;
          stream.finished(res, resolve);
        })
        .end();
      stream.finished(res, () => {
        strictEqual(closed, true);
      });
    });

    server.listen(8080);

    await env.SERVICE.fetch('https://cloudflare.com');
  },
};

// Test is taken from test/parallel/test-http-outgoing-end-types.js
export const testHttpOutgoingEndTypes = {
  async test(_ctrl, env) {
    await using httpServer = http.createServer(function (req, res) {
      throws(
        () => {
          res.end(['Throws.']);
        },
        {
          code: 'ERR_INVALID_ARG_TYPE',
        }
      );
      res.end();
    });

    httpServer.listen(8080);

    await env.SERVICE.fetch('https://cloudflare.com');
  },
};

// Test is taken from test/parallel/test-http-outgoing-first-chunk-singlebyte-encoding.js
export const testHttpOutgoingFirstChunkSingleByteEncoding = {
  async test(_ctrl, env) {
    // TODO(soon): Add utf-16 to this list when it's supported.
    for (const enc of ['utf8', 'latin1', 'UTF-8']) {
      await using server = http.createServer((req, res) => {
        res.setHeader('content-type', `text/plain; charset=${enc}`);
        res.write('helloworld', enc);
        res.end();
      });

      server.listen(8080);

      const res = await env.SERVICE.fetch('http://cloudflare.com', {
        headers: {
          'content-type': `text/plain; charset=${enc}`,
        },
      });
      strictEqual(res.status, 200);
      strictEqual(
        res.headers.get('content-type'),
        `text/plain; charset=${enc}`
      );
      const buf = await res.arrayBuffer();
      strictEqual(Buffer.from(buf, enc).toString(), 'helloworld');
    }
  },
};

export default httpServerHandler({ port: 8080 });

// Relevant Node.js tests
//
// - [x] test/parallel/test-http-outgoing-buffer.js
// - [x] test/parallel/test-http-outgoing-destroy.js
// - [x] test/parallel/test-http-outgoing-destroyed.js
// - [x] test/parallel/test-http-outgoing-end-multiple.js
// - [x] test/parallel/test-http-outgoing-end-types.js
// - [x] test/parallel/test-http-outgoing-first-chunk-singlebyte-encoding.js
// - [x] test/parallel/test-http-outgoing-finished.js
// - [x] test/parallel/test-http-outgoing-finish-writable.js
// - [x] test/parallel/test-http-outgoing-properties.js
// - [x] test/parallel/test-http-outgoing-proto.js
// - [x] test/parallel/test-http-outgoing-renderHeaders.js
// - [x] test/parallel/test-http-outgoing-writableFinished.js
// - [x] test/parallel/test-http-outgoing-write-types.js

// The following tests is not relevant to us:
//
// - [ ] test/parallel/test-http-outgoing-end-cork.js
// - [ ] test/parallel/test-http-outgoing-finish.js
// - [ ] test/parallel/test-http-outgoing-message-capture-rejection.js
// - [ ] test/parallel/test-http-outgoing-message-inheritance.js
// - [ ] test/parallel/test-http-outgoing-message-write-callback.js
// - [ ] test/parallel/test-http-outgoing-settimeout.js
