// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, ok } from 'node:assert';

// The sidecar sends a frame with the RSV2 bit set, which fails the WebSocket connection. The
// WebSocket standard then requires an error event followed by a close event with code 1006 and
// wasClean false. With the web_socket_close_after_error flag off, only the error event fires.

export default {
  async test(controller, env) {
    const closeAfterError =
      Cloudflare.compatibilityFlags['web_socket_close_after_error'];
    const ws = new WebSocket(
      `ws://${env.SIDECAR_HOSTNAME}:${env.RSV_SERVER_PORT}/`
    );

    const events = [];
    const done = new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => reject(new Error(`timed out; events: ${JSON.stringify(events)}`)),
        5000
      );
      const settle = () => {
        clearTimeout(timer);
        // Give a close event that must not fire a chance to arrive.
        setTimeout(resolve, 500);
      };
      ws.addEventListener('error', (event) => {
        events.push({ type: 'error', message: event.message });
        if (!closeAfterError) settle();
      });
      ws.addEventListener('close', (event) => {
        events.push({
          type: 'close',
          code: event.code,
          wasClean: event.wasClean,
          readyState: ws.readyState,
        });
        settle();
      });
    });
    await done;

    strictEqual(events[0].type, 'error');
    ok(
      events[0].message.includes('RSV bits 2 or 3'),
      `unexpected error message: ${events[0].message}`
    );
    if (closeAfterError) {
      strictEqual(events.length, 2, JSON.stringify(events));
      strictEqual(events[1].type, 'close');
      strictEqual(events[1].code, 1006);
      strictEqual(events[1].wasClean, false);
      strictEqual(events[1].readyState, WebSocket.CLOSED);
    } else {
      strictEqual(events.length, 1, JSON.stringify(events));
    }
  },
};
