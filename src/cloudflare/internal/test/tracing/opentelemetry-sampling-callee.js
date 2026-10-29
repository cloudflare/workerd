// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint, tracing } from 'cloudflare:workers';
import assert from 'node:assert';
import { trace } from '@opentelemetry/api';

export default {
  fetch(request, env) {
    const path = new URL(request.url).pathname;
    if (path === '/socket') {
      return env.SOCKET.get(env.SOCKET.idFromName('logger')).fetch(request);
    }
    const flags = tracing.getActiveSpan().spanContext().traceFlags;
    if (path.startsWith('/sampling-buffered-')) {
      const span = trace
        .getTracer('sampling-callee')
        .startSpan(`sampling-callee${path}`);
      assert.strictEqual(span.isRecording(), flags === 1);
      span.end();
    }
    return Response.json(flags);
  },
};

export class RpcLogger extends WorkerEntrypoint {
  check() {
    console.log('sampling-rpc-log');
    return tracing.getActiveSpan().spanContext().traceFlags;
  }
}

export class SocketLogger {
  constructor(state) {
    this.state = state;
  }
  fetch() {
    const [server, client] = Object.values(new WebSocketPair());
    this.state.acceptWebSocket(server);
    return new Response(null, { status: 101, webSocket: client });
  }
  webSocketMessage(socket) {
    tracing.enterSpan('sampling-websocket', () => {
      console.log('sampling-websocket-log');
    });
    socket.send('ok');
  }
  webSocketClose(socket) {
    socket.close(1000, 'done');
  }
}
