// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';

// The WebSocket standard says a message event's origin is the serialized origin of the socket's
// URL. Checking that needs a real connection, since a WebSocketPair endpoint has no URL, so this
// test talks to a sidecar server that sends one message on connect.
//
// env.MODE says which behavior to expect, because the
// spec_compliant_message_event_origin compat flag has no enable date.

async function firstMessageOrigin(env) {
  const address = `${env.SIDECAR_HOSTNAME}:${env.ORIGIN_SERVER_PORT}`;
  const ws = new WebSocket(`ws://${address}/chat`);
  try {
    const { promise, resolve } = Promise.withResolvers();
    ws.addEventListener('message', (event) => {
      resolve({ data: event.data, origin: event.origin });
    });
    return { address, ...(await promise) };
  } finally {
    ws.close();
  }
}

export const messageEventOrigin = {
  async test(ctrl, env) {
    strictEqual(typeof env.MODE, 'string');
    const { address, data, origin } = await firstMessageOrigin(env);

    strictEqual(data, 'hello');
    if (env.MODE === 'spec') {
      // The origin of the URL, so scheme and host but no path.
      strictEqual(origin, `ws://${address}`);
    } else {
      strictEqual(origin, null);
    }
  },
};
