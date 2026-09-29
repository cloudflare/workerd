// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The server captures its listeners' rejections. A 'request' listener's
// rejection tears its response down (see response-lifecycle.js); any other
// listener's rejection is reported the way EventEmitter's own capture
// fallback reports one: as an 'error' event, heard by an 'error' listener
// or thrown uncaught without one. An 'error' listener's own rejection is
// re-raised uncaught, once — never emitted again.

import http from 'node:http';
import { deepStrictEqual } from 'node:assert';
import { collectUncaught } from 'harness';

// Runs fn with a server whose 'listening' listener is async and rejects
// with `boom`, listening on an ephemeral port, then closes it.
async function withRejectingListeningListener(boom, setup) {
  const server = http.createServer(() => {});
  setup(server);
  server.on('listening', async () => {
    throw boom;
  });
  await new Promise((resolve) => server.listen(0, resolve));
  try {
    await scheduler.wait(10);
  } finally {
    server.close();
  }
}

// A 'listening' listener rejecting reaches the server's 'error' listener;
// nothing escapes the isolate.
export const otherListenerRejectionIsEmittedAsError = {
  async test() {
    const boom = new Error('listening rejected');
    const errors = [];
    const leaked = await collectUncaught(() =>
      withRejectingListeningListener(boom, (server) => {
        server.on('error', (err) => errors.push(err));
      })
    );
    deepStrictEqual(errors, [boom]);
    deepStrictEqual(leaked, []);
  },
};

// Without an 'error' listener the rejection is thrown uncaught, as an
// unlistened 'error' event is.
export const otherListenerRejectionWithoutErrorListenerIsUncaught = {
  async test() {
    const boom = new Error('listening rejected, unlistened');
    const leaked = await collectUncaught(() =>
      withRejectingListeningListener(boom, () => {})
    );
    deepStrictEqual(leaked, [boom]);
  },
};

// An async 'error' listener that itself rejects: its rejection is re-raised
// uncaught, once, and is not emitted as another 'error' (which would loop).
export const errorListenerRejectionIsUncaughtOnce = {
  async test() {
    const boom = new Error('listening rejected');
    const errorBoom = new Error('error listener rejected');
    const heard = [];
    const leaked = await collectUncaught(() =>
      withRejectingListeningListener(boom, (server) => {
        server.on('error', async (err) => {
          heard.push(err);
          throw errorBoom;
        });
      })
    );
    deepStrictEqual(heard, [boom]);
    deepStrictEqual(leaked, [errorBoom]);
  },
};
