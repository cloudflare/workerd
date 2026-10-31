import { WorkerEntrypoint, DurableObject } from 'cloudflare:workers';
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
      if (new URL(request.url).pathname === '/rpc')
        strictEqual(await env.PROBE.getCaller(), null);
      return Response.json({
        identity,
        body: request.bodyUsed ? null : await request.text(),
      });
    } catch (error) {
      return Response.json({ name: error.name, message: error.message });
    }
  },
};

export default handler;

export class Probe extends WorkerEntrypoint {
  getCaller() {
    return this.ctx.identity.caller();
  }
}
export class ReceiverDO extends DurableObject {
  fetch(request) {
    return handler.fetch(request, this.env, this.ctx);
  }
}
