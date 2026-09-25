// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

'use strict';

const assert = require('node:assert/strict');

const url = process.argv[2];
assert.ok(url, 'Usage: websocket-close-propagation-client.js <ws-url>');

const CLOSE_TIMEOUT_MS = 5000;
const CONNECT_RETRY_DEADLINE_MS = Date.now() + 5000;

function connectOnce() {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    let settled = false;

    const timeout = setTimeout(() => {
      if (!settled) {
        settled = true;
        reject(
          new Error(
            `Timed out after ${CLOSE_TIMEOUT_MS}ms waiting for close event`
          )
        );
      }
    }, CLOSE_TIMEOUT_MS);

    function done(fn) {
      if (!settled) {
        settled = true;
        clearTimeout(timeout);
        fn();
      }
    }

    ws.addEventListener('open', () => {
      ws.send('hello');
    });

    ws.addEventListener('message', (event) => {
      if (event.data !== 'Echo: hello') {
        done(() => reject(new Error(`Unexpected message: ${event.data}`)));
        return;
      }

      // The echo response above proves data flows end-to-end. Initiate a
      // clean close — the close event should propagate back to the client
      // the same way data does.
      ws.close(1000, 'client closing');
    });

    ws.addEventListener('close', (event) => {
      done(() => {
        if (event.code !== 1000) {
          reject(
            new Error(
              `Expected close code 1000, got ${event.code} (reason: ${event.reason})`
            )
          );
        } else {
          resolve({ code: event.code, reason: event.reason });
        }
      });
    });

    ws.addEventListener('error', (event) => {
      done(() => reject(new Error(event?.message ?? 'WebSocket error')));
    });
  });
}

(async () => {
  for (;;) {
    try {
      const result = await connectOnce();
      console.log(`Closed with code=${result.code} reason=${result.reason}`);
      process.exit(0);
    } catch (err) {
      const message = err?.message ?? String(err);
      const isConnectError =
        message.includes('ECONNREFUSED') ||
        message.includes('network error') ||
        message.includes('non-101');

      if (isConnectError && Date.now() < CONNECT_RETRY_DEADLINE_MS) {
        await new Promise((r) => setTimeout(r, 100));
        continue;
      }

      console.error(message);
      process.exit(1);
    }
  }
})();
