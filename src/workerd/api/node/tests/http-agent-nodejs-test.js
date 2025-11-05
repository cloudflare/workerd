// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import http from 'node:http';
import https from 'node:https';
import { strictEqual, ok } from 'node:assert';

export const checkPortsSetCorrectly = {
  test(_ctrl, env) {
    const keys = ['SIDECAR_HOSTNAME', 'PONG_SERVER_PORT'];
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
        host: env.SIDECAR_HOSTNAME,
      },
      resolve
    );
    await promise;
  },
};

// Test is taken from test/parallel/test-https-agent-constructor.js
export const testHttpsAgentConstructor = {
  async test() {
    ok(new https.Agent() instanceof https.Agent);
    strictEqual(typeof https.request, 'function');
    strictEqual(typeof http.get, 'function');
  },
};

// Tests covering http-agent
// - [ ] test/parallel/test-http-agent-abort-controller.js
// - [ ] test/parallel/test-http-agent-close.js
// - [ ] test/parallel/test-http-agent-destroyed-socket.js
// - [ ] test/parallel/test-http-agent-domain-reused-gc.js
// - [ ] test/parallel/test-http-agent-error-on-idle.js
// - [ ] test/parallel/test-http-agent-false.js
// - [x] test/parallel/test-http-agent-getname.js
// - [ ] test/parallel/test-http-agent-keepalive-delay.js
// - [ ] test/parallel/test-http-agent-keepalive.js
// - [ ] test/parallel/test-http-agent-maxsockets-respected.js
// - [ ] test/parallel/test-http-agent-maxsockets.js
// - [ ] test/parallel/test-http-agent-maxtotalsockets.js
// - [ ] test/parallel/test-http-agent-no-protocol.js
// - [x] test/parallel/test-http-agent-null.js
// - [ ] test/parallel/test-http-agent-remove.js
// - [ ] test/parallel/test-http-agent-reuse-drained-socket-only.js
// - [ ] test/parallel/test-http-agent-scheduling.js
// - [ ] test/parallel/test-http-agent-timeout-option.js
// - [ ] test/parallel/test-http-agent-timeout.js
// - [ ] test/parallel/test-http-agent-uninitialized-with-handle.js
// - [ ] test/parallel/test-http-agent.js
// - [ ] test/parallel/test-https-agent-abort-controller.js
// - [ ] test/parallel/test-https-agent-additional-options.js
// - [x] test/parallel/test-https-agent-constructor.js
// - [ ] test/parallel/test-https-agent-create-connection.js
// - [ ] test/parallel/test-https-agent-disable-session-reuse.js
// - [ ] test/parallel/test-https-agent-getname.js
// - [ ] test/parallel/test-https-agent-keylog.js
// - [ ] test/parallel/test-https-agent-servername.js
// - [ ] test/parallel/test-https-agent-session-eviction.js
// - [ ] test/parallel/test-https-agent-session-injection.js
// - [ ] test/parallel/test-https-agent-session-reuse.js
// - [ ] test/parallel/test-https-agent-sni.js
// - [ ] test/parallel/test-https-agent-sockets-leak.js
// - [ ] test/parallel/test-https-agent-unref-socket.js
// - [ ] test/parallel/test-https-agent.js

// Tests doesn't make sense for workerd:
//
// - [ ] test/parallel/test-http-agent-uninitialized.js
