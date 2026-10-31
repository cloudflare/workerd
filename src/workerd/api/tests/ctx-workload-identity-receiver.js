import { strictEqual } from 'node:assert';
export default {
  async fetch(request, env, ctx) {
    try {
      if (new URL(request.url).pathname === '/consumed') await request.text();
      if (new URL(request.url).pathname === '/locked') request.body.getReader();
      const first = ctx.identity.caller();
      strictEqual(first, ctx.identity.caller());
      const identity = await first;
      if (identity) strictEqual(Object.isFrozen(identity), true);
      strictEqual(identity, await ctx.identity.caller());
      return Response.json({
        identity,
        body: request.bodyUsed ? null : await request.text(),
      });
    } catch (error) {
      return Response.json({ name: error.name, message: error.message });
    }
  },
};
