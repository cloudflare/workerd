// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests the cap on how deeply blockConcurrencyWhile() calls may be nested. Nesting beyond
// MAX_BLOCK_CONCURRENCY_WHILE_DEPTH throws synchronously.
import assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

// The limit enforced in io-context.h. A call whose critical-section depth is <= this is allowed;
// the first call past it throws.
const MAX_DEPTH = 64;

export class DepthTester extends DurableObject {
  // Enters `enterCount` nested blockConcurrencyWhile() critical sections (so the innermost runs at
  // critical-section depth `enterCount`), then attempts one more blockConcurrencyWhile() and
  // reports whether that final "probe" call threw. The probe is wrapped in try/catch so that, even
  // when it throws, the surrounding critical sections unwind cleanly and the input gate is not
  // broken.
  async probeAtDepth(enterCount) {
    const recurse = async (remaining) => {
      if (remaining > 0) {
        return await this.ctx.blockConcurrencyWhile(() =>
          recurse(remaining - 1)
        );
      }
      try {
        await this.ctx.blockConcurrencyWhile(() => 'ok');
        return { threw: false };
      } catch (e) {
        return { threw: true, message: e.message };
      }
    };
    return await recurse(enterCount);
  }
}

export default {
  async test(_request, env) {
    {
      // Entering `MAX_DEPTH - 1` critical sections puts the probe at exactly MAX_DEPTH, which is
      // still allowed.
      const result = await env.ns
        .getByName('at-limit')
        .probeAtDepth(MAX_DEPTH - 1);
      assert.strictEqual(result.threw, false);
    }

    {
      // Entering `MAX_DEPTH` critical sections puts the probe at MAX_DEPTH + 1, which exceeds the
      // limit and must throw.
      const result = await env.ns
        .getByName('over-limit')
        .probeAtDepth(MAX_DEPTH);
      assert.strictEqual(result.threw, true);
      assert.match(
        result.message,
        /blockConcurrencyWhile\(\) calls are nested too deeply\./
      );
    }
  },
};
