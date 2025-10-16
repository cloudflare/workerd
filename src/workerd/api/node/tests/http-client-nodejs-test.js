// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import http from 'node:http';
import { strictEqual, ok, throws, deepStrictEqual } from 'node:assert';
import { once } from 'node:events';
import { get } from 'node:http';

export const checkPortsSetCorrectly = {
  test(_ctrl, env) {
    const keys = [
      'SIDECAR_HOSTNAME',
      'PONG_SERVER_PORT',
      'ASD_SERVER_PORT',
      'DEFAULT_HEADERS_EXIST_PORT',
      'REQUEST_ARGUMENTS_PORT',
      'HELLO_WORLD_SERVER_PORT',
    ];
    for (const key of keys) {
      strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
  },
};

// TODO(soon): This is triggering io-context error. Fix this edge case.
// Test is taken from Node.js: test/parallel/test-http-client-request-options.js
// export const testHttpClientRequestOptions = {
//   async test(_ctrl, env) {
//     const headers = { foo: 'Bar' };
//     const { promise, resolve, reject } = Promise.withResolvers();
//     const url = new URL(`http://localhost:${env.PONG_SERVER_PORT}/ping?q=term`);
//     url.headers = headers;
//     const clientReq = http.request(url);
//     clientReq.on('close', resolve);
//     clientReq.on('error', reject)
//     clientReq.end();
//     await promise;
//   },
// };

// Test is taken from test/parallel/test-http-client-res-destroyed.js
export const testHttpClientResDestroyed = {
  async test(_ctrl, env) {
    {
      const { promise, resolve } = Promise.withResolvers();
      http.get(
        {
          hostname: env.SIDECAR_HOSTNAME,
          port: env.ASD_SERVER_PORT,
        },
        (res) => {
          strictEqual(res.destroyed, false);
          res.destroy();
          strictEqual(res.destroyed, true);
          res.on('close', resolve);
        }
      );
      await promise;
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      http.get(
        {
          hostname: env.SIDECAR_HOSTNAME,
          port: env.ASD_SERVER_PORT,
        },
        (res) => {
          strictEqual(res.destroyed, false);
          res
            .on('close', () => {
              strictEqual(res.destroyed, true);
              resolve();
            })
            .resume();
        }
      );
      await promise;
    }
  },
};

// TODO(soon): Support this test case, if possible with the current implementation
// Test is taken from test/parallel/test-http-client-response-timeout.js
// export const testHttpClientResponseTimeout = {
//   async test(_ctrl, env) {
//     const { promise, resolve } = Promise.withResolvers();
//     const req =
//       http.get({ port: env.TIMEOUT_SERVER_PORT }, (res) => {
//         res.on('timeout', () => {
//           resolve();
//           req.destroy();
//         });
//         res.setTimeout(1);
//       });
//     await promise;
//   },
// };

// TODO(soon): Handle this edge case.
// Test is taken from test/parallel/test-http-client-close-event.js
// export const testHttpClientCloseEvent = {
//   async test(_ctrl, env) {
//     const { promise, resolve, reject } = Promise.withResolvers();
//     const req = http.get({ port: env.PONG_SERVER_PORT }, () => {
//       reject(new Error('Should not have called this callback'));
//     });

//     const errFn = mock.fn((err) => {
//       strictEqual(err.constructor, Error);
//       strictEqual(err.message, 'socket hang up');
//       strictEqual(err.code, 'ECONNRESET');
//     });
//     req.on('error', errFn);

//     req.on('close', () => {
//       strictEqual(req.destroyed, true);
//       strictEqual(errFn.mock.callCount(), 1);
//       resolve();
//     });

//     req.destroy();
//     await promise;
//   },
// };

// Test is taken from test/parallel/test-http-client-default-headers-exist.js
export const testHttpClientDefaultHeadersExist = {
  async test(_ctrl, env) {
    const expectedHeaders = {
      DELETE: ['host', 'connection'],
      GET: ['host', 'connection'],
      HEAD: ['host', 'connection'],
      OPTIONS: ['host', 'connection'],
      POST: ['host', 'connection', 'content-length'],
      PUT: ['host', 'connection', 'content-length'],
      TRACE: ['host', 'connection'],
    };

    const expectedMethods = Object.keys(expectedHeaders);

    await Promise.all(
      expectedMethods.map(async (method) => {
        const request = http
          .request({
            method: method,
            hostname: env.SIDECAR_HOSTNAME,
            port: env.DEFAULT_HEADERS_EXIST_PORT,
          })
          .end();
        return once(request, 'response');
      })
    );
  },
};

// Test is taken from test/parallel/test-http-client-defaults.js
export const testHttpClientDefaults = {
  async test() {
    {
      const req = new http.ClientRequest({});
      strictEqual(req.path, '/');
      strictEqual(req.method, 'GET');
    }

    {
      const req = new http.ClientRequest({ method: '' });
      strictEqual(req.path, '/');
      strictEqual(req.method, 'GET');
    }

    {
      const req = new http.ClientRequest({ path: '' });
      strictEqual(req.path, '/');
      strictEqual(req.method, 'GET');
    }
  },
};

// Test is taken from test/parallel/test-http-client-encoding.js
export const testHttpClientEncoding = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    http
      .request(
        {
          hostname: env.SIDECAR_HOSTNAME,
          port: env.PONG_SERVER_PORT,
          encoding: 'utf8',
        },
        (res) => {
          let data = '';
          res.on('data', (chunk) => (data += chunk));
          res.on('end', () => {
            strictEqual(data, 'pong');
            resolve();
          });
        }
      )
      .end();
    await promise;
  },
};

// Test is taken from test/parallel/test-http-client-headers-host-array.js
export const testHttpClientHeadersHostArray = {
  async test() {
    const options = {
      port: '80',
      path: '/',
      headers: {
        host: [],
      },
    };

    throws(
      () => {
        http.request(options);
      },
      {
        code: /ERR_INVALID_ARG_TYPE/,
      },
      'http request should throw when passing array as header host'
    );
  },
};

// Test is taken from test/parallel/test-http-client-input-function.js
export const testHttpClientInputFunction = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    const req = new http.ClientRequest(
      { hostname: env.SIDECAR_HOSTNAME, port: env.PONG_SERVER_PORT },
      (response) => {
        let body = '';
        response.setEncoding('utf8');
        response.on('data', (chunk) => {
          body += chunk;
        });

        response.on('end', () => {
          strictEqual(body, 'pong');
          resolve();
        });
      }
    );

    req.end();
    await promise;
  },
};

// Test is taken from test/parallel/test-http-client-invalid-path.js
export const testHttpClientInvalidPath = {
  async test() {
    throws(
      () => {
        http
          .request({
            path: '/thisisinvalid\uffe2',
          })
          .end();
      },
      {
        code: 'ERR_UNESCAPED_CHARACTERS',
        name: 'TypeError',
      }
    );
  },
};

// Test is taken from test/parallel/test-http-client-unescaped-path.js
export const testHttpClientUnescapedPath = {
  async test() {
    for (let i = 0; i <= 32; i += 1) {
      const path = `bad${String.fromCharCode(i)}path`;
      throws(
        () =>
          http.get({ path }, () => {
            throw new Error('This should not happen');
          }),
        {
          code: 'ERR_UNESCAPED_CHARACTERS',
          name: 'TypeError',
          message: 'Request path contains unescaped characters',
        }
      );
    }
  },
};

// Test is taken from test/parallel/test-http-request-arguments.js
export const testHttpRequestArguments = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    http.get(
      'http://example.com/testpath',
      { hostname: env.SIDECAR_HOSTNAME, port: env.REQUEST_ARGUMENTS_PORT },
      (res) => {
        res.resume();
        resolve();
      }
    );
    await promise;
  },
};

// Test is taken from test/parallel/test-http-request-dont-override-options.js
export const testHttpRequestDontOverrideOptions = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    const agent = new http.Agent();
    agent.defaultPort = env.PONG_SERVER_PORT;

    // Options marked as explicitly undefined for readability
    // in this test, they should STAY undefined as options should not
    // be mutable / modified
    const options = {
      host: undefined,
      hostname: env.SIDECAR_HOSTNAME,
      port: undefined,
      defaultPort: undefined,
      path: undefined,
      method: undefined,
      agent: agent,
    };

    http
      .request(options, function (res) {
        res.resume();
        strictEqual(options.host, undefined);
        strictEqual(options.hostname, env.SIDECAR_HOSTNAME);
        strictEqual(options.port, undefined);
        strictEqual(options.defaultPort, undefined);
        strictEqual(options.path, undefined);
        strictEqual(options.method, undefined);
        resolve();
      })
      .end();
    await promise;
  },
};

// Test is taken from test/parallel/test-http-request-host-header.js
export const testHttpRequestHostHeader = {
  async test(_ctrl, env) {
    // From RFC 7230 5.4 https://datatracker.ietf.org/doc/html/rfc7230#section-5.4
    // A server MUST respond with a 400 (Bad Request) status code to any
    // HTTP/1.1 request message that lacks a Host header field
    const { promise, resolve } = Promise.withResolvers();
    http.get(
      {
        hostname: env.SIDECAR_HOSTNAME,
        port: env.HEADER_VALIDATION_SERVER_PORT,
        headers: [],
      },
      (res) => {
        strictEqual(res.statusCode, 400);
        strictEqual(res.headers.connection, 'close');
        resolve();
      }
    );
    await promise;
  },
};

// Test is taken from test/parallel/test-http-request-invalid-method-error.js
export const testHttpRequestInvalidMethodError = {
  async test() {
    throws(() => http.request({ method: '\0' }), {
      code: 'ERR_INVALID_HTTP_TOKEN',
      name: 'TypeError',
      message: 'Method must be a valid HTTP token ["\u0000"]',
    });
  },
};

// Test is taken from test/parallel/test-http-request-join-authorization-headers.js
export const testHttpRequestJoinAuthorizationHeaders = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    http.get(
      {
        hostname: env.SIDECAR_HOSTNAME,
        port: env.HELLO_WORLD_SERVER_PORT,
        method: 'POST',
        headers: [
          'authorization',
          '1',
          'authorization',
          '2',
          'cookie',
          'foo',
          'cookie',
          'bar',
        ],
        joinDuplicateHeaders: true,
        path: '/join-duplicate-headers',
      },
      (res) => {
        strictEqual(res.statusCode, 200);
        strictEqual(res.headers.authorization, '3, 4');
        strictEqual(res.headers.cookie, 'foo; bar');
        resolve();
      }
    );
    await promise;
  },
};

export const testGetExport = {
  async test() {
    const { get: getFn } = await import('node:http');
    deepStrictEqual(get, getFn);
  },
};

export const testPostRequestBodyEcho = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const testData =
      'Hello, this is test data for POST request with body echoing!';

    const req = http.request(
      {
        hostname: env.SIDECAR_HOSTNAME,
        port: env.HELLO_WORLD_SERVER_PORT,
        path: '/echo',
        method: 'POST',
        headers: {
          'Content-Type': 'text/plain',
          'Content-Length': Buffer.byteLength(testData),
        },
      },
      (res) => {
        let responseBody = '';
        res.on('data', (chunk) => {
          responseBody += chunk.toString();
        });
        res.on('end', () => {
          try {
            strictEqual(res.statusCode, 200);
            strictEqual(
              responseBody,
              testData,
              `Response body should match the request body. Expected: ${JSON.stringify(testData)}, Got: ${JSON.stringify(responseBody)}`
            );
            resolve();
          } catch (err) {
            reject(err);
          }
        });
      }
    );

    req.on('error', (err) => {
      reject(new Error(`Request failed: ${err.message}`));
    });

    req.write(testData);
    req.end();

    await promise;
  },
};

export const testHttpGetTestSearchParams = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    http.get(
      {
        hostname: env.SIDECAR_HOSTNAME,
        port: env.HELLO_WORLD_SERVER_PORT,
        method: 'POST',
        path: '/search-path?hello=world',
      },
      (res) => {
        strictEqual(res.statusCode, 200);
        strictEqual(res.headers.url, '/search-path?hello=world');
        resolve();
      }
    );
    await promise;
  },
};

// Regression test for https://github.com/cloudflare/workerd/issues/5148
export const testBodyDataDuplicationRegression = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();

    http
      .request(
        {
          hostname: env.SIDECAR_HOSTNAME,
          port: env.HELLO_WORLD_SERVER_PORT,
          path: '/echo',
          method: 'post',
          headers: {
            'content-type': 'application/json;charset=utf-8',
          },
        },
        (response) => {
          strictEqual(response.statusCode, 200);
          let expected = '';
          response.on('data', (chunk) => (expected += chunk));
          response.on('end', () => {
            strictEqual(
              expected,
              '{"email":"posting-wrangler@email.mail","from":"wrangler"}'
            );
            resolve(response);
          });
        }
      )
      .on('error', reject)
      .end(
        Buffer.from(
          JSON.stringify({
            email: 'posting-wrangler@email.mail',
            from: 'wrangler',
          })
        )
      );

    await promise;
  },
};

// Relevant Node.js tests
// - [ ] test/parallel/test-http-client-abort-destroy.js
// - [ ] test/parallel/test-http-client-abort-event.js
// - [ ] test/parallel/test-http-client-abort-keep-alive-destroy-res.js
// - [ ] test/parallel/test-http-client-abort-keep-alive-queued-tcp-socket.js
// - [ ] test/parallel/test-http-client-abort-keep-alive-queued-unix-socket.js
// - [ ] test/parallel/test-http-client-abort-no-agent.js
// - [ ] test/parallel/test-http-client-abort-response-event.js
// - [ ] test/parallel/test-http-client-abort-unix-socket.js
// - [ ] test/parallel/test-http-client-abort.js
// - [ ] test/parallel/test-http-client-abort2.js
// - [ ] test/parallel/test-http-client-abort3.js
// - [ ] test/parallel/test-http-client-aborted-event.js
// - [ ] test/parallel/test-http-client-agent-abort-close-event.js
// - [ ] test/parallel/test-http-client-agent-end-close-event.js
// - [ ] test/parallel/test-http-client-agent.js
// - [ ] test/parallel/test-http-client-check-http-token.js
// - [x] test/parallel/test-http-client-close-event.js
// - [ ] test/parallel/test-http-client-close-with-default-agent.js
// - [x] test/parallel/test-http-client-default-headers-exist.js
// - [x] test/parallel/test-http-client-defaults.js
// - [x] test/parallel/test-http-client-encoding.js
// - [ ] test/parallel/test-http-client-finished.js
// - [ ] test/parallel/test-http-client-get-url.js
// - [ ] test/parallel/test-http-client-headers-array.js
// - [x] test/parallel/test-http-client-headers-host-array.js
// - [ ] test/parallel/test-http-client-incomingmessage-destroy.js
// - [x] test/parallel/test-http-client-input-function.js
// - [x] test/parallel/test-http-client-invalid-path.js
// - [ ] test/parallel/test-http-client-keep-alive-hint.js
// - [ ] test/parallel/test-http-client-keep-alive-release-before-finish.js
// - [ ] test/parallel/test-http-client-override-global-agent.js
// - [ ] test/parallel/test-http-client-race-2.js
// - [ ] test/parallel/test-http-client-race.js
// - [ ] test/parallel/test-http-client-read-in-error.js
// - [ ] test/parallel/test-http-client-readable.js
// - [ ] test/parallel/test-http-client-reject-chunked-with-content-length.js
// - [ ] test/parallel/test-http-client-reject-cr-no-lf.js
// - [ ] test/parallel/test-http-client-reject-unexpected-agent.js
// - [ ] test/parallel/test-http-client-req-error-dont-double-fire.js
// - [x] test/parallel/test-http-client-request-options.js
// - [x] test/parallel/test-http-client-res-destroyed.js
// - [x] test/parallel/test-http-client-response-timeout.js
// - [x] test/parallel/test-http-client-set-timeout.js
// - [ ] test/parallel/test-http-client-spurious-aborted.js
// - [ ] test/parallel/test-http-client-timeout-agent.js
// - [ ] test/parallel/test-http-client-timeout-connect-listener.js
// - [ ] test/parallel/test-http-client-timeout-event.js
// - [ ] test/parallel/test-http-client-timeout-on-connect.js
// - [ ] test/parallel/test-http-client-timeout-option-listeners.js
// - [ ] test/parallel/test-http-client-timeout-option-with-agent.js
// - [ ] test/parallel/test-http-client-timeout-option.js
// - [ ] test/parallel/test-http-client-timeout-with-data.js
// - [ ] test/parallel/test-http-client-timeout.js
// - [x] test/parallel/test-http-client-unescaped-path.js
// - [x] test/parallel/test-http-request-arguments.js
// - [x] test/parallel/test-http-request-dont-override-options.js
// - [ ] test/parallel/test-http-request-end-twice.js
// - [ ] test/parallel/test-http-request-end.js
// - [x] test/parallel/test-http-request-host-header.js
// - [x] test/parallel/test-http-request-invalid-method-error.js
// - [x] test/parallel/test-http-request-join-authorization-headers.js
// - [ ] test/parallel/test-http-request-large-payload.js
// - [ ] test/parallel/test-http-request-smuggling-content-length.js

// Tests doesn't make sense for workerd:
// - test/parallel/test-http-client-error-rawbytes.js
// - test/parallel/test-http-client-immediate-error.js
// - test/parallel/test-http-client-insecure-http-parser-error.js
// - test/parallel/test-http-client-parse-error.js
// - test/parallel/test-http-client-pipe-end.js
// - test/parallel/test-http-client-response-domain.js
// - test/parallel/test-http-client-set-timeout-after-end.js
// - test/parallel/test-http-client-upload-buf.js
// - test/parallel/test-http-client-upload.js
// - test/parallel/test-http-client-with-create-connection.js
// - test/parallel/test-http-request-method-delete-payload.js
// - test/parallel/test-http-request-methods.js
