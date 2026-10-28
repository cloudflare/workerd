// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';
import { DurableObject } from 'cloudflare:workers';
import { onHibernate } from 'cloudflare:durable-objects';

// Records whether the runtime looks up the onHibernate handler, which it must not do without the
// experimental flag.
export class RecordingReadObject extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    return new Proxy(this, {
      get(target, prop, receiver) {
        if (prop.description === 'cloudflare:durable-objects:onHibernate') {
          target.handlerRead = true;
        }
        return Reflect.get(target, prop, receiver);
      },
    });
  }

  async fetch() {
    return new Response(String(this.handlerRead === true));
  }
}

export const symbolRequiresExperimental = {
  test() {
    strictEqual(onHibernate, undefined);
  },
};

export const handlerIsNotReadWithoutExperimental = {
  async test(ctrl, env) {
    const response = await env.recordingRead.getByName('a').fetch('http://x/');
    strictEqual(await response.text(), 'false');
  },
};
