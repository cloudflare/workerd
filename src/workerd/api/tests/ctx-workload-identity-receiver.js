// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';
import { strictEqual } from 'node:assert';
const handler = {
  async fetch(request, env, ctx) {
    try {
      if (new URL(request.url).pathname === '/consumed') await request.text();
      if (new URL(request.url).pathname === '/locked') request.body.getReader();
      const first = ctx.identity.caller();
      strictEqual(first, ctx.identity.caller());
      const identity = await first;
      if (identity) strictEqual(Object.isFrozen(identity), true);
      strictEqual(identity, await ctx.identity.caller());
      const body = request.bodyUsed ? null : await request.text();
      if (new URL(request.url).pathname === '/empty') {
        strictEqual(request.body === null, false);
        strictEqual(request.bodyUsed, true);
        return new Response(null, {
          headers: {
            'x-verified-method': identity.method,
            'x-verified-body': identity.body,
            'x-original-body': body,
          },
        });
      }
      return Response.json({
        identity,
        body,
      });
    } catch (error) {
      return Response.json({ name: error.name, message: error.message });
    }
  },
};

export default handler;

export class ConstructedReceiver extends WorkerEntrypoint {
  constructor(ctx, env) {
    super(ctx, env);
    this.constructorCaller = ctx.identity.caller();
    this.constructorCaller.catch(() => {});
  }

  fetch(request) {
    strictEqual(this.constructorCaller, this.ctx.identity.caller());
    return handler.fetch(request, this.env, this.ctx);
  }
}

export class ReceiverDO extends DurableObject {
  fetch(request) {
    return handler.fetch(request, this.env, this.ctx);
  }
}
