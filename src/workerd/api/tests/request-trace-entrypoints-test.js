// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { tracing } from 'cloudflare:workers';

function check(name) {
  const active = tracing.getActiveSpan();
  assert.ok(active, name);
  const identity = active.spanContext();
  assert.match(identity.traceId, /^[0-9a-f]{32}$/);
  assert.match(identity.spanId, /^[0-9a-f]{16}$/);
  console.log(JSON.stringify({ name, ...identity }));
  tracing.startSpan(name).end();
  return identity;
}

export class TraceActor {
  constructor(state) {
    this.state = state;
    const constructor = check('entry-constructor');
    state.blockConcurrencyWhile(async () => {
      await Promise.resolve();
      assert.deepStrictEqual(check('entry-block'), constructor);
    });
  }
  fetch() {
    const { 0: client, 1: server } = new WebSocketPair();
    this.state.acceptWebSocket(server);
    return new Response(null, { status: 101, webSocket: client });
  }
  webSocketMessage(socket) {
    check('entry-message');
    socket.send('ok');
  }
  webSocketClose(socket) {
    check('entry-close');
    socket.close(1000, 'done');
  }
}

export default {
  fetch(request, env) {
    return env.NS.get(env.NS.idFromName('entrypoints')).fetch(request);
  },
};
