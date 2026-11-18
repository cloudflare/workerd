// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A stateless entrypoint's handler gets a new ctx, with that event's props, for every event, even
// though the runtime reuses its internal copy of the handler from one event to the next.

import { deepStrictEqual, notStrictEqual, strictEqual } from 'node:assert';

const seen = [];

export default {
  fetch(request, env, ctx) {
    seen.push(ctx);
    // ctx.props is the same object every time it is read, even when created on first read.
    strictEqual(ctx.props, ctx.props);
    if (new URL(request.url).pathname === '/nested') {
      // This ctx stays in use while another event runs.
      return (async () => {
        deepStrictEqual(
          await (await env.B.fetch('http://placeholder/')).json(),
          { b: 2 }
        );
        notStrictEqual(seen.at(-1), ctx);
        return Response.json(ctx.props);
      })();
    }
    return Response.json(ctx.props);
  },

  async test(ctrl, env, ctx) {
    seen.length = 0;
    const calls = [
      [env.A, { a: 1 }],
      [env.B, { b: 2 }],
      [env.A, { a: 1 }],
      [env.PLAIN, {}],
    ];
    for (const [service, props] of calls) {
      deepStrictEqual(
        await (await service.fetch('http://placeholder/')).json(),
        props
      );
    }

    deepStrictEqual(
      await (await env.A.fetch('http://placeholder/nested')).json(),
      { a: 1 }
    );

    strictEqual(seen.length, calls.length + 2);
    strictEqual(new Set(seen).size, seen.length);
    for (const eventCtx of seen) {
      notStrictEqual(eventCtx, ctx);
    }
  },
};
