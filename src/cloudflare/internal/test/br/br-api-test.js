// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

// Sends one message on an accepted socket and resolves with the mock's parsed echo.
function roundTrip(webSocket, message) {
  return new Promise((resolve, reject) => {
    webSocket.addEventListener('message', (event) => {
      resolve(JSON.parse(event.data));
    });
    webSocket.addEventListener('error', reject);
    webSocket.send(message);
  });
}

export const tests = {
  async test(_, env) {
    {
      // Test legacy fetch
      const resp = await env.browser.fetch('http://workers-binding.brapi/run', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({
          inputs: { test: true },
        }),
      });
      assert.deepStrictEqual(await resp.json(), { success: true });
    }
  },
};

export const passThroughMethods = {
  async test(_, env) {
    assert.strictEqual(env.browser.constructor.name, 'BrowserRendering');

    const resp = await env.browser.quickAction('screenshot', {
      url: 'https://example.com',
    });
    assert.deepStrictEqual(await resp.json(), {
      action: 'screenshot',
      url: 'https://example.com',
    });

    // Results keep RPC promise pipelining.
    assert.strictEqual(
      typeof (await env.browser.acquire().sessionId),
      'string'
    );

    const { sessionId } = await env.browser.acquire();
    assert.deepStrictEqual(await env.browser.getSession(sessionId), {
      sessionId,
    });
    assert.ok(
      (await env.browser.listSessions()).some((s) => s.sessionId === sessionId)
    );
    assert.ok(
      (await env.browser.history()).some((s) => s.sessionId === sessionId)
    );
    assert.deepStrictEqual(await env.browser.limits(), {
      maxConcurrentSessions: 10,
    });
    assert.deepStrictEqual(
      await env.browser.getLiveView(sessionId, { mode: 'devtools' }),
      { id: sessionId, options: { mode: 'devtools' } }
    );
    assert.deepStrictEqual(await env.browser.devtools.listTargets(sessionId), [
      { id: `${sessionId}-page`, type: 'page', url: 'about:blank' },
    ]);
    assert.deepStrictEqual(await env.browser.closeSession(sessionId), {
      status: 'closed',
    });
    assert.strictEqual(await env.browser.getSession(sessionId), null);
  },
};

export const launchReturnsFetcherByDefault = {
  async test(_, env) {
    for (const options of [undefined, {}, { websocket: false }]) {
      const connection = await env.browser.launch(options);
      assert.strictEqual(typeof connection.sessionId, 'string');
      assert.ok(!(connection.webSocket instanceof WebSocket));

      const resp = await connection.webSocket.fetch(
        'https://browser-binding.invalid/',
        { headers: { Upgrade: 'websocket' } }
      );
      assert.strictEqual(resp.status, 101);
      resp.webSocket.accept();
      resp.webSocket.close();
    }
  },
};

export const launchWithWebSocket = {
  async test(_, env) {
    const connection = await env.browser.launch({
      targets: true,
      websocket: true,
    });
    assert.ok(connection.webSocket instanceof WebSocket);
    assert.deepStrictEqual(connection.targets, [
      {
        id: `${connection.sessionId}-page`,
        type: 'page',
        url: 'about:blank',
      },
    ]);
    // The upgrade keeps the RPC result object, so it stays disposable.
    assert.strictEqual(typeof connection[Symbol.dispose], 'function');

    connection.webSocket.accept();
    const reply = await roundTrip(connection.webSocket, 'hello');
    assert.deepStrictEqual(reply, {
      sessionId: connection.sessionId,
      targetId: null,
      echo: 'hello',
    });
    connection.webSocket.close();
  },
};

export const connectSessionWithWebSocket = {
  async test(_, env) {
    const { sessionId } = await env.browser.acquire();

    const connection = await env.browser.connectSession(sessionId, {
      targetId: 'page-1',
      websocket: true,
    });
    assert.strictEqual(connection.sessionId, sessionId);
    assert.ok(connection.webSocket instanceof WebSocket);

    connection.webSocket.accept();
    const reply = await roundTrip(connection.webSocket, 'hello');
    assert.deepStrictEqual(reply, {
      sessionId,
      targetId: 'page-1',
      echo: 'hello',
    });
    connection.webSocket.close();

    const fetcherConnection = await env.browser.connectSession(sessionId, {
      websocket: false,
    });
    assert.ok(!(fetcherConnection.webSocket instanceof WebSocket));
  },
};

export const failedUpgradeClosesLaunchedSession = {
  async test(_, env) {
    const before = await env.browser.listSessions();

    await assert.rejects(
      env.browser.launch({ location: 'fail-upgrade', websocket: true }),
      {
        message:
          'Browser Run WebSocket upgrade failed with HTTP 503: upgrade refused',
      }
    );

    assert.deepStrictEqual(await env.browser.listSessions(), before);
  },
};

export const failedUpgradeKeepsConnectedSession = {
  async test(_, env) {
    const { sessionId } = await env.browser.acquire({
      location: 'fail-upgrade',
    });

    await assert.rejects(
      env.browser.connectSession(sessionId, { websocket: true }),
      {
        message:
          'Browser Run WebSocket upgrade failed with HTTP 503: upgrade refused',
      }
    );

    assert.deepStrictEqual(await env.browser.getSession(sessionId), {
      sessionId,
    });
  },
};
