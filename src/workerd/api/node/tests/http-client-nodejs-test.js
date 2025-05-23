// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import http from 'node:http';
import { strictEqual, ok, deepStrictEqual, rejects } from 'node:assert';

export const checkPortsSetCorrectly = {
  test(_ctrl, env) {
    const keys = [
      'PONG_SERVER_PORT',
      'ASD_SERVER_PORT',
      'TIMEOUT_SERVER_PORT',
      'HELLO_WORLD_SERVER_PORT',
      'HEADER_VALIDATION_SERVER_PORT',
    ];
    for (const key of keys) {
      strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
  },
};

// Test is taken from Node.js: test/parallel/test-http-client-request-options.js
export const testHttpClientRequestOptions = {
  async test(_ctrl, env) {
    const headers = { foo: 'Bar' };
    const { promise, resolve } = Promise.withResolvers();
    const url = new URL(`http://localhost:${env.PONG_SERVER_PORT}/ping?q=term`);
    url.headers = headers;
    const clientReq = http.request(url);
    clientReq.on('close', resolve);
    clientReq.end();
    await promise;
  },
};

// Test is taken from test/parallel/test-http-client-res-destroyed.js
export const testHttpClientResDestroyed = {
  async test(_ctrl, env) {
    {
      const { promise, resolve } = Promise.withResolvers();
      http.get(
        {
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

// Test is taken from test/parallel/test-http-content-length.js
export const testHttpContentLength = {
  async test(_ctrl, env) {
    const expectedHeadersEndWithData = {
      connection: 'keep-alive',
      'content-length': String('hello world'.length),
    };

    const expectedHeadersEndNoData = {
      connection: 'keep-alive',
      'content-length': '0',
    };

    const { promise, resolve } = Promise.withResolvers();
    let req;

    req = http.request({
      port: env.HELLO_WORLD_SERVER_PORT,
      method: 'POST',
      path: '/end-with-data',
    });
    req.removeHeader('Date');
    req.end('hello world');
    req.on('response', function (res) {
      deepStrictEqual(res.headers, {
        ...expectedHeadersEndWithData,
        'keep-alive': 'timeout=1',
      });
      res.resume();
    });

    req = http.request({
      port: env.HELLO_WORLD_SERVER_PORT,
      method: 'POST',
      path: '/empty',
    });
    req.removeHeader('Date');
    req.end();
    req.on('response', function (res) {
      deepStrictEqual(res.headers, {
        ...expectedHeadersEndNoData,
        'keep-alive': 'timeout=1',
      });
      res.resume();
      resolve();
    });
    await promise;
  },
};

// Test is taken from test/parallel/test-http-contentLength0.js
export const testHttpContentLength0 = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const request = http.request(
      {
        port: env.HELLO_WORLD_SERVER_PORT,
        method: 'POST',
        path: '/content-length0',
      },
      (response) => {
        response.on('error', reject);
        response.resume();
        response.on('end', resolve);
      }
    );
    request.on('error', rejects);
    request.end();
    await promise;
  },
};

// Test is taken from test/parallel/test-http-dont-set-default-headers-with-set-header.js
export const testHttpDontSetDefaultHeadersWithSetHeader = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const req = http.request({
      method: 'POST',
      port: env.HEADER_VALIDATION_SERVER_PORT,
      setDefaultHeaders: false,
      path: '/test-1',
    });

    req.setHeader('test', 'value');
    req.setHeader('HOST', `localhost:${env.HEADER_VALIDATION_SERVER_PORT}`);
    req.setHeader('foo', ['bar', 'baz']);
    req.setHeader('connection', 'close');
    req.on('response', resolve);
    req.on('error', reject);

    req.end();
    await promise;
  },
};

// Test is taken from test/parallel/test-http-dont-set-default-headers-with-setHost.js
export const testHttpDontSetDefaultHeadersWithSetHost = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    http
      .request({
        method: 'POST',
        port: env.HEADER_VALIDATION_SERVER_PORT,
        setDefaultHeaders: false,
        setHost: true,
        path: '/test-2',
      })
      .on('error', reject)
      .on('response', resolve)
      .end();
    await promise;
  },
};
