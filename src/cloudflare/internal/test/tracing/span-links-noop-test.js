// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';
import { tracing } from 'cloudflare:workers';

const startup = tracing.startSpan('startup');

export const noops = {
  test(ctrl, env, ctx) {
    const span = ctx.tracing.startSpan('untraced');
    for (const receiver of [startup, span]) {
      assert.equal(receiver.isTraced, false);
      assert.equal(typeof receiver.addLink, 'function');
      assert.equal(typeof receiver.addLinks, 'function');
      const poison = {
        get context() {
          throw new Error('no-op span read its input');
        },
      };
      assert.equal(receiver.addLink(poison), receiver);
      assert.equal(receiver.addLinks([poison]), receiver);
      receiver.end();
    }
  },
};
