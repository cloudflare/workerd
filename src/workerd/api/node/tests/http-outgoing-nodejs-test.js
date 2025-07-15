import http from 'node:http';
import { throws, strictEqual, ok } from 'node:assert';
import { mock } from 'node:test';
import { Writable } from 'node:stream';

export const checkPortsSetCorrectly = {
  test(_ctrl, env) {
    const keys = [
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

// TODO(soon): Implement _write method of OutgoingMessage
// Tests are taken from test/parallel/test-http-outgoing-message-inheritance.js
// export const testHttpOutgoingMessageInheritance = {
//   async test() {
//     // Check that http.OutgoingMessage can be used without a proper Socket
//     // Refs: https://github.com/nodejs/node/issues/14386
//     // Refs: https://github.com/nodejs/node/issues/14381
//     const { promise, resolve } = Promise.withResolvers();

//     class Response extends http.OutgoingMessage {
//       _implicitHeader() {}
//     }

//     const res = new Response();

//     let firstChunk = true;
//     const writeFn = mock.fn((chunk, encoding, callback) => {
//       if (firstChunk) {
//         ok(chunk.toString().endsWith('hello world'));
//         firstChunk = false;
//       } else {
//         strictEqual(chunk.length, 0);
//       }
//       setImmediate(callback);

//       if (writeFn.mock.callCount() === 2) {
//         resolve();
//       }
//     });

//     const ws = new Writable({
//       write: writeFn,
//     });

//     res.socket = ws;
//     ws._httpMessage = res;
//     res.connection = ws;

//     res.end('hello world');
//     await promise;
//   }
// }

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
        port: env.PROPERTIES_PORT,
        method: 'GET',
        path: '/',
      });

      strictEqual(req.path, '/');
      strictEqual(req.method, 'GET');
      strictEqual(req.host, 'localhost');
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

// Relevant Node.js tests
//
// - [x] test/parallel/test-http-outgoing-destroy.js
// - [ ] test/parallel/test-http-outgoing-destroyed.js
// - [ ] test/parallel/test-http-outgoing-end-cork.js
// - [ ] test/parallel/test-http-outgoing-end-multiple.js
// - [x] test/parallel/test-http-outgoing-finish-writable.js
// - [ ] test/parallel/test-http-outgoing-message-capture-rejection.js
// - [ ] test/parallel/test-http-outgoing-message-inheritance.js
// - [x] test/parallel/test-http-outgoing-properties.js
// - [x] test/parallel/test-http-outgoing-proto.js
// - [x] test/parallel/test-http-outgoing-renderHeaders.js
// - [x] test/parallel/test-http-outgoing-writableFinished.js

// The following tests is not relevant to us:
//
// - [ ] test/parallel/test-http-outgoing-buffer.js
// - [ ] test/parallel/test-http-outgoing-end-types.js
// - [ ] test/parallel/test-http-outgoing-finish.js
// - [ ] test/parallel/test-http-outgoing-finished.js
// - [ ] test/parallel/test-http-outgoing-first-chunk-singlebyte-encoding.js
// - [ ] test/parallel/test-http-outgoing-message-write-callback.js
// - [ ] test/parallel/test-http-outgoing-settimeout.js
// - [ ] test/parallel/test-http-outgoing-write-types.js
