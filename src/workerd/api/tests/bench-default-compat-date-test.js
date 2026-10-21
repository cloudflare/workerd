// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  bench(b, _env, ctx) {
    // `ctx.exports` exists from enable_ctx_exports' date, 2025-11-17, so it shows that the worker
    // got a recent date rather than an old one.
    if (ctx.exports === undefined) {
      throw new Error(
        'Expected the newest compat date, which enables ctx.exports.'
      );
    }
    b.run('noop', () => b.blackBox(1));
  },
};
