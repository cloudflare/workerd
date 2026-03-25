// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

export const ctxAccessPropertyExists = {
  test(controller, env, ctx) {
    // The access property is always present on ctx as a lazy instance property.
    // In standalone workerd no AccessInfo is supplied to newWorkerEntrypoint(), so the
    // current IncomingRequest has no AccessInfo and getAccess() returns kj::none, which
    // surfaces as `undefined` to JS.
    strictEqual('access' in ctx, true);
    strictEqual(ctx.access, undefined);
  },
};
