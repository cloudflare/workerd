// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import http from 'node:http';

export const checkPortsSetCorrectly = {
  test(_ctrl, env) {
    const keys = ['PONG_SERVER_PORT'];
    for (const key of keys) {
      strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
  },
};

// Test is taken from Node.js: test/parallel/test-http-client-request-options.js
export const testHttpClientRequestOptions = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    const url = new URL(`http://localhost:${env.PONG_SERVER_PORT}/ping?q=term`);
    url.headers = headers;
    const clientReq = http.request(url);
    clientReq.on('close', resolve);
    clientReq.end();
    await promise;
  },
};
