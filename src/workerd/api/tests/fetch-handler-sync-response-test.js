// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A fetch handler that returns a Response itself, rather than a promise for one, has it sent
// without awaiting a promise. These cases check that it behaves as a handler returning a promise
// for the same Response does.

import { strictEqual } from 'node:assert';

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
    default:
      throw new Error(`unknown path ${path}`);
  }
}

export default {
  fetch(request) {
    const [, mode, path] = new URL(request.url).pathname.split('/');
    const response = respond(path);
    return mode === 'async' ? Promise.resolve(response) : response;
  },
};

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
      const sync = await outcome(env, 'sync', path);
      strictEqual(sync, await outcome(env, 'async', path), path);
    }
    strictEqual(await outcome(env, 'sync', 'plain'), '200 plain null');
    strictEqual(await outcome(env, 'sync', 'microtask'), '200 microtask ran');
    strictEqual(await outcome(env, 'sync', 'stream'), '200 streamed null');
  },
};
