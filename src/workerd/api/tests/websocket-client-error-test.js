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
          'Uncaught Error: WebSocket protocol error; protocolError.statusCode = 1009; protocolError.description = Message is too large: 2097152 > 1048576'
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
        if (!(receivedError && event.code === 1009)) {
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
  },
};
