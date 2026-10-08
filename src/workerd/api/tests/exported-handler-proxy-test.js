// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';

// The handler names the runtime may read from every entrypoint when it delivers an event. Some
// Workers return a Proxy from their entrypoint's constructor that rejects any other property, so
// reading a new name here breaks them.
const HANDLER_NAMES = new Set([
  'alarm',
  'connect',
  'email',
  'fetch',
  'queue',
  'scheduled',
  // ExportedHandler's `self` is a SelfRef. The struct wrapper ignores the value, but still reads
  // the property.
  'self',
  'tail',
  'tailStream',
  'test',
  'trace',
  'webSocketClose',
  'webSocketError',
  'webSocketMessage',
]);

export class Strict extends WorkerEntrypoint {
  constructor(ctx, env) {
    super(ctx, env);
    return new Proxy(this, {
      get(target, prop) {
        if (Reflect.has(target, prop)) return Reflect.get(target, prop);
        if (typeof prop === 'string' && HANDLER_NAMES.has(prop))
          return undefined;
        throw new Error(`unexpected property read: ${String(prop)}`);
      },
    });
  }

  fetch() {
    return new Response('ok');
  }
}

export const fetchThroughStrictProxy = {
  async test(ctrl, env) {
    const response = await env.STRICT.fetch('http://example.com/');
    if ((await response.text()) !== 'ok') {
      throw new Error('fetch through a strict Proxy entrypoint failed');
    }
  },
};
