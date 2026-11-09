// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { DurableObject } from 'cloudflare:workers';
import { onHibernate } from 'cloudflare:durable-objects';
import unsafe from 'workerd:unsafe';

export class OnHibernateObject extends DurableObject {
  fetch() {
    return new Response();
  }

  [onHibernate]() {}
}

// A test entrypoint named `test` isn't traced, so only the object's events reach the tails.
export const test = {
  async test(ctrl, env) {
    const stub = env.ns.getByName('a');
    await stub.fetch('http://x/');
    // Runs the onHibernate handler before shutting the object down.
    await unsafe.evict(stub);
  },
};
