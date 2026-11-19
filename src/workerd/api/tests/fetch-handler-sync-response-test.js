// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A fetch handler that returns a Response itself, or a promise already fulfilled with one, has it
// sent without awaiting a promise. These cases check that it behaves as a handler returning a
// promise that is still pending does.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

function respond(path) {
  switch (path) {
    case 'plain':
      return new Response('plain');
    case 'microtask': {
      // The handler's microtasks run before the Response is sent.
      const response = new Response('microtask');
      queueMicrotask(() => response.headers.set('x-microtask', 'ran'));
      return response;
    }
    case 'stream': {
      // A body written after the handler returns.
      const { readable, writable } = new IdentityTransformStream();
      const writer = writable.getWriter();
      writer.write(new TextEncoder().encode('streamed'));
      writer.close();
      return new Response(readable);
    }
    case 'error':
      return Response.error();
    case 'wrong':
      return 42;
    default:
      throw new Error(`unknown path ${path}`);
  }
}

export default {
  fetch(request) {
    const [, mode, path] = new URL(request.url).pathname.split('/');
    const response = respond(path);
    switch (mode) {
      case 'sync':
        return response;
      case 'async':
        return Promise.resolve(response);
      case 'pending':
        return (async () => {
          await null;
          return response;
        })();
      default:
        throw new Error(`unknown mode ${mode}`);
    }
  },
};

// An actor's Response, returned itself or as a promise, waits for its output locks, here held by
// the write the handler leaves unawaited, and is then sent.
export class Counter extends DurableObject {
  count = 0;

  fetch(request) {
    this.count += 1;
    this.ctx.storage.put('count', this.count);
    const response = new Response(`${this.count}`);
    return new URL(request.url).pathname === '/async'
      ? Promise.resolve(response)
      : response;
  }

  async stored() {
    return await this.ctx.storage.get('count');
  }
}

async function outcome(env, mode, path) {
  try {
    const response = await env.SELF.fetch(`http://example.com/${mode}/${path}`);
    const microtask = response.headers.get('x-microtask');
    return `${response.status} ${await response.text()} ${microtask}`;
  } catch (e) {
    return `${e.name}: ${e.message}`;
  }
}

export const syncResponseMatchesAsync = {
  async test(ctrl, env) {
    for (const path of ['plain', 'microtask', 'stream', 'error']) {
      const pending = await outcome(env, 'pending', path);
      strictEqual(await outcome(env, 'sync', path), pending, path);
      strictEqual(await outcome(env, 'async', path), pending, path);
    }
    strictEqual(await outcome(env, 'sync', 'plain'), '200 plain null');
    strictEqual(await outcome(env, 'sync', 'microtask'), '200 microtask ran');
    strictEqual(await outcome(env, 'sync', 'stream'), '200 streamed null');
  },
};

// A value that is not a Response fails as it always has, whether the handler returned it or a
// promise for it.
export const wrongTypeFails = {
  async test(ctrl, env) {
    const outcomes = [];
    for (const mode of ['sync', 'async', 'pending']) {
      outcomes.push(await outcome(env, mode, 'wrong'));
    }
    const promised =
      "TypeError: Incorrect type for Promise: the Promise did not resolve to 'Response'.";
    deepStrictEqual(outcomes, [
      "TypeError: Callback returned incorrect type; expected 'Response or Promise'",
      promised,
      promised,
    ]);
  },
};

export const actorSyncResponse = {
  async test(ctrl, env) {
    const stub = env.COUNTER.getByName('counter');
    strictEqual(
      await (await stub.fetch('http://example.com/sync')).text(),
      '1'
    );
    strictEqual(
      await (await stub.fetch('http://example.com/async')).text(),
      '2'
    );
    strictEqual(
      await (await stub.fetch('http://example.com/sync')).text(),
      '3'
    );
    strictEqual(await stub.stored(), 3);
  },
};
