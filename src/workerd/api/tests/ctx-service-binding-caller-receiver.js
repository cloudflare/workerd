// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint, RpcTarget } from 'cloudflare:workers';
import { strictEqual } from 'node:assert';
async function who(ctx) {
  try {
    const first = ctx.identity.caller();
    strictEqual(first, ctx.identity.caller());
    await Promise.resolve();
    const caller = await first;
    strictEqual(Object.isFrozen(caller), true);
    strictEqual(caller, await ctx.identity.caller());
    return caller;
  } catch (error) {
    return { error: error.name };
  }
}
class Target extends RpcTarget {
  constructor(ctx) {
    super();
    this.ctx = ctx;
  }
  who() {
    return who(this.ctx);
  }
}
export class Named extends WorkerEntrypoint {
  who() {
    return who(this.ctx);
  }
  async fetch(request) {
    const body = await request.text();
    return Response.json({ caller: await who(this.ctx), body });
  }
  hop() {
    return this.env.C.who();
  }
  hopB() {
    return this.env.B.who();
  }
  target() {
    return new Target(this.ctx);
  }
  invoke(binding) {
    return binding.who();
  }
  invokeFetch(binding) {
    return binding
      .fetch('https://b.example', {
        headers: { authorization: 'Bearer forged' },
      })
      .then((r) => r.json());
  }
  callback(fn) {
    return fn();
  }
  loopback() {
    return this.ctx.exports.Named.who();
  }
}
export default class extends Named {}
