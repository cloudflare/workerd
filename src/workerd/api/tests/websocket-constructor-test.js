// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, throws } from 'node:assert';

// Test that WebSocket constructor handles empty protocols array correctly.
// Per the WebSocket spec, an empty protocols array should be valid and equivalent
// to not specifying any protocols.
// See: https://github.com/cloudflare/workerd/issues/5822
export const emptyProtocolsArray = {
  async test() {
    const ws = new WebSocket('wss://example.com/', []);
    strictEqual(ws.url, 'wss://example.com/');
    strictEqual(ws.protocol, '');
    strictEqual(ws.readyState, WebSocket.CONNECTING);
    ws.close();
  },
};

// Test that a single protocol string works
export const singleProtocolString = {
  async test() {
    const ws = new WebSocket('wss://example.com/', 'chat');
    strictEqual(ws.url, 'wss://example.com/');
    ws.close();
  },
};

// Test that invalid protocol tokens still throw SyntaxError
export const invalidProtocolToken = {
  async test() {
    throws(
      () => new WebSocket('wss://example.com/', 'invalid protocol with spaces'),
      {
        name: 'SyntaxError',
      }
    );
  },
};

// Test that duplicate protocols throw SyntaxError
export const duplicateProtocols = {
  async test() {
    throws(() => new WebSocket('wss://example.com/', ['chat', 'chat']), {
      name: 'SyntaxError',
    });
  },
};
