import http from 'node:http';
import { strictEqual } from 'node:assert';

// Tests are taken from test/parallel/test-http-agent-false.js
export const testHttpAgentFalse = {
  async test() {
    const { promise, resolve, reject } = Promise.withResolvers();
    // Sending `agent: false` when `port: null` is also passed in (i.e. the result
    // of a `url.parse()` call with the default port used, 80 or 443), should not
    // result in an assertion error...
    const opts = {
      host: '127.0.0.1',
      port: null,
      path: '/',
      method: 'GET',
      agent: false,
    };

    // We just want an "error" (no local HTTP server on port 80) or "response"
    // to happen (user happens ot have HTTP server running on port 80).
    // As long as the process doesn't crash from a C++ assertion then we're good.
    const req = http.request(opts);

    // Will be called by either the response event or error event, not both
    req.on('response', resolve);
    req.on('error', reject);
    req.end();
    await promise;
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
