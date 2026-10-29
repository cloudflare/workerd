// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import {
  DurableObject,
  RpcStub,
  RpcTarget,
  WorkerEntrypoint,
  restore,
  tracing,
} from 'cloudflare:workers';

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
  async webSocketMessage(socket, message) {
    if (message === 'check') {
      check('entry-message');
      socket.send('ok');
    } else if (message === 'first') {
      const identity = check('entry-first-before');
      this.firstReady = Promise.withResolvers();
      this.firstDone = Promise.withResolvers();
      socket.send('first-started');
      await this.firstReady.promise;
      assert.deepStrictEqual(check('entry-first-after'), identity);
      socket.send('first-done');
      this.firstDone.resolve();
    } else if (message === 'second') {
      const identity = check('entry-second-before');
      // Keep this newer request alive while the first request's continuation runs.
      this.firstReady.resolve();
      await this.firstDone.promise;
      assert.deepStrictEqual(check('entry-second-after'), identity);
      socket.send('second-done');
    }
  }
  webSocketClose(socket) {
    check('entry-close');
    socket.close(1000, 'done');
  }
}

export class RestoreSource extends WorkerEntrypoint {
  async mint(kind) {
    this.minting = true;
    return await this.ctx.restore({ kind });
  }

  async [restore]({ kind }) {
    // Persisted stubs replay on a fresh entrypoint, outside the mint() RPC frame.
    if (!this.minting) {
      const identity = check(`entry-restore-${kind}`);
      await Promise.resolve();
      assert.deepStrictEqual(check(`entry-restore-${kind}-after`), identity);
    }
    return kind === 'service'
      ? this.ctx.exports.RestoreTarget({})
      : new RpcStub(new RestoredRpcTarget());
  }
}

export class RestoreTarget extends WorkerEntrypoint {
  ping() {
    return 'restored';
  }
}

class RestoredRpcTarget extends RpcTarget {
  ping() {
    return 'restored';
  }
}

export class TraceStore extends DurableObject {
  async storeStub(stub) {
    await this.ctx.storage.put('stub', stub);
  }

  async invoke() {
    const stub = await this.ctx.storage.get('stub');
    return await stub.ping();
  }
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname.startsWith('/restore/')) {
      const kind = url.pathname.slice('/restore/'.length);
      const stub = await ctx.exports.RestoreSource.mint(kind);
      const store = ctx.exports.TraceStore.getByName(kind);
      await store.storeStub(stub);
      return new Response(await store.invoke());
    }
    return env.NS.get(env.NS.idFromName('entrypoints')).fetch(request);
  },
};
