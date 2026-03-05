// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, doesNotThrow } from 'node:assert';

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

// Test that calling close() inside the close event handler after the automatic close
// handshake is silently ignored (doesn't throw). This is the realistic pattern for
// users who are already manually replying to close frames today — when they update
// their compat date and get web_socket_auto_reply_to_close, their existing close()
// call must not break.
export const closeInsideHandlerAfterAutoCloseIsIgnored = {
  async test() {
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);

    client.accept();
    server.accept();

    const closePromise = new Promise((resolve) => {
      client.addEventListener('close', (event) => {
        // This is what existing user code typically looks like: replying to close
        // inside the close handler. With the auto-reply already sent, closedOutgoing
        // is true but the native state is still Accepted (tryReleaseNative hasn't
        // run yet), so this exercises the closedOutgoing early-return in close().
        doesNotThrow(() => {
          client.close(1000, 'manual reply inside handler');
        });

        resolve({
          readyState: client.readyState,
          code: event.code,
          wasClean: event.wasClean,
        });
      });
    });

    // Server initiates close; the runtime auto-replies because allowHalfOpen is false.
    server.close(1000, 'server closing');

    const result = await closePromise;
    strictEqual(result.readyState, WebSocket.CLOSED);
    strictEqual(result.code, 1000);
    strictEqual(result.wasClean, true);
  },
};

// Same as above, but calling close() after the handler has returned and the native
// WebSocket has been released. This exercises the state.is<Released>() early-return
// in close(), which is a different code path from the closedOutgoing check above.
export const closeAfterAutoCloseAndReleaseIsIgnored = {
  async test() {
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);

    client.accept();
    server.accept();

    const closePromise = new Promise((resolve) => {
      client.addEventListener('close', (event) => {
        resolve({
          readyState: client.readyState,
          code: event.code,
          wasClean: event.wasClean,
        });
      });
    });

    // Server initiates close; the runtime auto-replies because allowHalfOpen is false.
    server.close(1000, 'server closing');

    const result = await closePromise;
    strictEqual(result.readyState, WebSocket.CLOSED);

    // By now tryReleaseNative has run and the state is Released.
    // close() should still be silently ignored.
    doesNotThrow(() => {
      client.close(1000, 'manual reply after release');
    });

    strictEqual(client.readyState, WebSocket.CLOSED);
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
