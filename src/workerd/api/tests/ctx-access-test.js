// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

export const ctxAccessPropertyExists = {
  test(controller, env, ctx) {
    // The access property is always present on ctx as a lazy instance property.
    // In standalone workerd (no embedding override), the value is undefined because
    // the default Worker::Api::getCtxAccessProperty() returns kj::none.
    strictEqual('access' in ctx, true);
    strictEqual(ctx.access, undefined);
  },
};
