// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import http from 'node:http';
import { strictEqual, ok } from 'node:assert';

export const checkPortsSetCorrectly = {
  test(_ctrl, env) {
    const keys = ['PONG_SERVER_PORT'];
    for (const key of keys) {
      strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
  },
};

// Test is taken from test/parallel/test-http-agent-getname.js
export const testHttpAgentGetName = {
  async test() {
    const agent = new http.Agent();

    // Default to localhost
    strictEqual(
      agent.getName({
        port: 80,
        localAddress: '192.168.1.1',
      }),
      'localhost:80:192.168.1.1'
    );

    // empty argument
    strictEqual(agent.getName(), 'localhost::');

    // empty options
    strictEqual(agent.getName({}), 'localhost::');

    // pass all arguments
    strictEqual(
      agent.getName({
        host: '0.0.0.0',
        port: 80,
        localAddress: '192.168.1.1',
      }),
      '0.0.0.0:80:192.168.1.1'
    );

    // unix socket
    strictEqual(
      agent.getName({
        socketPath: '/tmp/foo/bar',
      }),
      `localhost:::/tmp/foo/bar`
    );

    for (const family of [0, null, undefined, 'bogus'])
      strictEqual(agent.getName({ family }), 'localhost::');

    for (const family of [4, 6])
      strictEqual(agent.getName({ family }), `localhost:::${family}`);
  },
};

// Test is taken from test/parallel/test-http-agent-null.js
export const testHttpAgentNull = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    http.get(
      {
        agent: null,
        port: env.PONG_SERVER_PORT,
      },
      resolve
    );
    await promise;
  },
};
