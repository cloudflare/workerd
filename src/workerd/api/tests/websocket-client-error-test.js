// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This test verifies that WebSocket protocol errors are properly converted to JavaScript errors
// using the JsgifyWebSocketErrors handler.

export default {
  async test(controller, env) {
    const ws = new WebSocket(
      `ws://${env.SIDECAR_HOSTNAME}:${env.BIG_MESSAGE_SERVER_PORT}/`
    );
    let receivedError = false;
    let closeEvent;

    // Set up a promise to wait for errors
    const errorPromise = new Promise((resolve, reject) => {
      // Set a timeout
      const timeoutId = setTimeout(() => {
        reject({
          source: 'timeout',
          message: 'No error received within timeout',
        });
      }, 5000);

      // Handle errors
      ws.addEventListener('error', (event) => {
        clearTimeout(timeoutId);
        receivedError = true;

        if (
          event.message ===
          'Uncaught Error: WebSocket protocol error; protocolError.statusCode = 1009; protocolError.description = Message is too large: 34603008 > 33554432'
        ) {
          resolve({
            source: 'client-error',
            message: event.message,
          });
        } else {
          reject({
            source: 'client-error-malformed',
            message: event.message,
          });
        }
      });

      ws.addEventListener('open', () => {});
      // Handle close
      ws.addEventListener('close', (event) => {
        closeEvent = event;
        if (!(receivedError && event.code === 1006)) {
          clearTimeout(timeoutId);
          reject({
            source: 'client-close',
            code: event.code,
            reason: event.reason,
          });
        }
      });
    });

    // Wait for an error or timeout
    await errorPromise;

    // With web_socket_close_after_error the failed connection also closes, with 1006 because no
    // Close frame was received; the error event is dispatched first.
    if (Cloudflare.compatibilityFlags['web_socket_close_after_error']) {
      await new Promise((resolve) => setTimeout(resolve, 500));
      if (closeEvent?.code !== 1006 || closeEvent.wasClean !== false) {
        throw new Error(
          `expected close 1006 after the error, got ${JSON.stringify(closeEvent && { code: closeEvent.code, wasClean: closeEvent.wasClean })}`
        );
      }
    }
  },
};
