// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, throws } from 'node:assert';

// Runs in two Workers, one with websocket_standard_url_and_extensions and one without, which
// `env.standard` tells apart. Both connect through the global outbound to
// websocket-standard-url-server.js, which answers any message with the URL the WebSocket was
// requested with.

// Waits for `ws` to open, and resolves to the URL the server saw.
function open(ws) {
  return new Promise((resolve, reject) => {
    ws.addEventListener('open', () => ws.send('url'));
    ws.addEventListener('message', (event) => resolve(event.data));
    ws.addEventListener('error', (event) => reject(event.error));
  });
}

export const httpSchemes = {
  async test(ctrl, env) {
    if (!env.standard) {
      for (const url of ['http://example.com/', 'https://example.com/']) {
        throws(() => new WebSocket(url), { name: 'SyntaxError' }, url);
      }
      return;
    }

    const ws = new WebSocket('http://example.com/a');
    strictEqual(ws.url, 'ws://example.com/a');
    strictEqual(await open(ws), 'http://example.com/a');
    ws.close();

    const wss = new WebSocket('https://user:pass@example.com/b?c');
    strictEqual(wss.url, 'wss://user:pass@example.com/b?c');
    // The userinfo is not sent.
    strictEqual(await open(wss), 'https://example.com/b?c');
    wss.close();
  },
};

export const urlIsSerialized = {
  async test(ctrl, env) {
    const bare = new WebSocket('wss://EXAMPLE.com');
    strictEqual(
      bare.url,
      env.standard ? 'wss://example.com/' : 'wss://EXAMPLE.com'
    );
    bare.close();

    const ws = new WebSocket('wss://EXAMPLE.com/a/../b');
    strictEqual(
      ws.url,
      env.standard ? 'wss://example.com/b' : 'wss://EXAMPLE.com/a/../b'
    );
    strictEqual(await open(ws), 'https://example.com/b');
    ws.close();
  },
};

export const relativeUrlThrows = {
  async test() {
    for (const url of ['', 'test', '/path', '?']) {
      throws(() => new WebSocket(url), { name: 'SyntaxError' }, url);
    }
  },
};

export const invalidUrlsThrow = {
  async test(ctrl, env) {
    const urls = [
      'ftp://example.com/',
      'wss://example.com/#',
      'wss://example.com/#frag',
    ];
    if (env.standard) {
      // The URL parser rejects a special URL with no host.
      urls.push('ws://');
    }
    for (const url of urls) {
      throws(() => new WebSocket(url), { name: 'SyntaxError' }, url);
    }
  },
};

export const protocolAndExtensionsAfterOpen = {
  async test(ctrl, env) {
    const ws = new WebSocket('wss://example.com/');
    strictEqual(ws.protocol, '');
    strictEqual(ws.extensions, '');
    await open(ws);
    const expected = env.standard ? '' : null;
    strictEqual(ws.protocol, expected);
    strictEqual(ws.extensions, expected);
    ws.close();
  },
};

export const repeatedProtocolsDifferentCase = {
  test(ctrl, env) {
    const protocols = ['echo', 'Echo'];
    if (env.standard) {
      throws(() => new WebSocket('wss://example.com/', protocols), {
        name: 'SyntaxError',
      });
    } else {
      new WebSocket('wss://example.com/', protocols).close();
    }
    throws(() => new WebSocket('wss://example.com/', ['echo', 'echo']), {
      name: 'SyntaxError',
    });
  },
};
