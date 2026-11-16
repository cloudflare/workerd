// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Durable Object whose onHibernate handler outlasts its budget, in a worker
// of its own because the handler needs the experimental compatibility flag.
//   /on-hibernate/start    Starts the object.
//   /on-hibernate/evict    Evicts the object, which runs its handler.
//   /on-hibernate/started  Whether the handler has started.

import { DurableObject } from 'cloudflare:workers';
import { onHibernate } from 'cloudflare:durable-objects';
import unsafe from 'workerd:unsafe';

let handlerStarted = false;

export class Sleeper extends DurableObject {
  fetch() {
    return new Response('running');
  }

  async [onHibernate]() {
    handlerStarted = true;
    await scheduler.wait(60_000);
  }
}

export default {
  async fetch(request, env) {
    const stub = env.SLEEPER.getByName('sleeper');
    switch (new URL(request.url).pathname) {
      case '/on-hibernate/start':
        return stub.fetch(request);
      case '/on-hibernate/evict':
        try {
          await unsafe.evict(stub);
          return new Response('evicted');
        } catch (err) {
          return new Response(String(err), { status: 500 });
        }
      case '/on-hibernate/started':
        return Response.json(handlerStarted);
    }
    return new Response('Not found', { status: 404 });
  },
};
