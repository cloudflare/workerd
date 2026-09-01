// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

export class EventSourceGcDo extends DurableObject {
  async trigger() {
    await this.ctx.blockConcurrencyWhile(async () => {
      // EventSource delivery acquires the actor input gate, so messages stay queued until this
      // callback returns.
      const response = await this.env.backend.fetch('https://example.com');
      const ended = Promise.withResolvers();
      let source = EventSource.from(response.body);
      source.onerror = () => ended.resolve();

      await ended.promise;

      const sourceRef = new WeakRef(source);
      source = null;
      // Queue stackless GC before yielding so its task runs before this request resumes.
      const gcPromise = gc({ type: 'major', execution: 'async' });
      await scheduler.wait(0);
      await gcPromise;
      assert.strictEqual(sourceRef.deref(), undefined);
    });

    await scheduler.wait(0);
  }
}

export default {
  async test(_controller, env) {
    await env.ns.getByName('test').trigger();
  },
  fetch() {
    return new Response('data: message\n\n');
  },
};
