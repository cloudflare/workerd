// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, throws } from 'node:assert';

// send() on a `new WebSocket()` that is still CONNECTING. With pedantic_wpt it throws an
// InvalidStateError DOMException, per https://websockets.spec.whatwg.org/#dom-websocket-send;
// without it, the TypeError workerd has always thrown.
export const sendWhileConnecting = {
  async test(ctrl, env) {
    const ws = new WebSocket('wss://example.com/');
    strictEqual(ws.readyState, WebSocket.CONNECTING);
    if (env.pedantic) {
      throws(() => ws.send('text'), {
        name: 'InvalidStateError',
        code: DOMException.INVALID_STATE_ERR,
      });
    } else {
      throws(() => ws.send('text'), TypeError);
    }
    strictEqual(ws.readyState, WebSocket.CONNECTING);
    strictEqual(ws.bufferedAmount, 0);
    ws.close();
  },
};

// A WebSocketPair end that hasn't been accepted has no CONNECTING state in the spec, so send()
// keeps throwing TypeError either way.
export const sendBeforeAccept = {
  async test() {
    const [client, server] = Object.values(new WebSocketPair());
    throws(() => server.send('text'), TypeError);
    throws(() => client.send('text'), TypeError);
  },
};
