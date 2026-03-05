// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

// Test that when allowHalfOpen is false (default with compat flag), a server-initiated
// close sets readyState to CLOSED.
export const autoCloseReplyWhenNotHalfOpen = {
  async test() {
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);

    // accept() without options — allowHalfOpen defaults to false with the compat flag.
    client.accept();
    server.accept();

    const closePromise = new Promise((resolve) => {
      client.addEventListener('close', (event) => {
        // When allowHalfOpen is false, the runtime auto-sends a close reply,
        // so both closedIncoming and closedOutgoing are true — readyState should be CLOSED.
        resolve({
          readyState: client.readyState,
          code: event.code,
          wasClean: event.wasClean,
        });
      });
    });

    // Server initiates close.
    server.close(1000, 'server closing');

    const result = await closePromise;
    strictEqual(result.readyState, WebSocket.CLOSED);
    strictEqual(result.code, 1000);
    strictEqual(result.wasClean, true);
  },
};

// Test that when allowHalfOpen is true via accept(), a server-initiated close sets
// readyState to CLOSING.
export const halfOpenCloseKeepsClosingState = {
  async test() {
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);

    // Opt into half-open mode via accept() options.
    client.accept({ allowHalfOpen: true });
    server.accept();

    const closePromise = new Promise((resolve) => {
      client.addEventListener('close', (event) => {
        // When allowHalfOpen is true, no auto-reply is sent, so only closedIncoming
        // is true — readyState should be CLOSING.
        resolve({
          readyState: client.readyState,
          code: event.code,
          wasClean: event.wasClean,
        });
      });
    });

    // Server initiates close.
    server.close(1000, 'server closing');

    const result = await closePromise;
    strictEqual(result.readyState, WebSocket.CLOSING);
    strictEqual(result.code, 1000);
    strictEqual(result.wasClean, true);

    // The client must manually close to complete the handshake.
    client.close(1000, 'client reply');
  },
};
