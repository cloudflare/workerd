// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects, strictEqual } from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

export class FailingActor extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    if (ctx.id.name.startsWith('constructor')) {
      ctx.blockConcurrencyWhile(async () => {
        if (ctx.id.name.includes('logging')) {
          console.log('before rejection 1');
          console.log('before rejection 2');
        }
        throw new Error('constructor failure');
      });
    }
  }

  fetch() {
    throw new Error('fetch failure');
  }
}

const cases = ['constructor-silent', 'constructor-logging', 'fetch'];
const requests = [undefined, { headers: { Upgrade: 'websocket' } }];

export const handledRejection = {
  async test(_controller, env) {
    const events = [];
    const handler = (event) => {
      events.push(event);
      event.preventDefault();
    };
    addEventListener('unhandledrejection', handler);
    try {
      for (const name of cases) {
        for (const [index, init] of requests.entries()) {
          const stub = env.ns.getByName(`${name}-handled-${index}`);
          await rejects(stub.fetch('https://example.com', init), {
            message: name.startsWith('constructor')
              ? 'constructor failure'
              : 'fetch failure',
          });
          await new Promise((resolve) => setTimeout(resolve, 10));
          strictEqual(events.length, 0, `${name}: no unhandled rejection`);
        }
      }
    } finally {
      removeEventListener('unhandledrejection', handler);
    }
  },
};

export const unhandledRejection = {
  async test(_controller, env) {
    for (const name of cases) {
      for (const [index, init] of requests.entries()) {
        const events = [];
        const { promise: reported, resolve } = Promise.withResolvers();
        const handler = (event) => {
          events.push(event);
          event.preventDefault();
          resolve();
        };
        addEventListener('unhandledrejection', handler);
        const stub = env.ns.getByName(`${name}-unhandled-${index}`);
        const promise = stub.fetch('https://example.com', init);
        try {
          await reported;
          await new Promise((resolve) => setTimeout(resolve, 10));
          strictEqual(events.length, 1, `${name}: one unhandled rejection`);
          strictEqual(events[0].promise, promise);
          strictEqual(
            events[0].reason.message,
            name.startsWith('constructor')
              ? 'constructor failure'
              : 'fetch failure'
          );
        } finally {
          promise.catch(() => {});
          removeEventListener('unhandledrejection', handler);
        }
      }
    }
  },
};
