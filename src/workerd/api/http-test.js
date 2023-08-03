// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "node:assert";

let scheduledLastCtrl;

export default {
  async scheduled(ctrl, env, ctx) {
    scheduledLastCtrl = ctrl;
    if (ctrl.cron === "* * * * 30") ctrl.noRetry();
  },

  async test(ctrl, env, ctx) {
    // Call `scheduled()` with no options
    {
      const result = await env.SERVICE.scheduled();
      assert.strictEqual(result.outcome, "ok");
      assert(!result.noRetry);
      assert(Math.abs(Date.now() - scheduledLastCtrl.scheduledTime) < 3_000);
      assert.strictEqual(scheduledLastCtrl.cron, "");
    }

    // Call `scheduled()` with options, and noRetry()
    {
      const result = await env.SERVICE.scheduled({ scheduledTime: 1000, cron: "* * * * 30" });
      assert.strictEqual(result.outcome, "ok");
      assert(result.noRetry);
      assert.strictEqual(scheduledLastCtrl.scheduledTime, 1000);
      assert.strictEqual(scheduledLastCtrl.cron, "* * * * 30");
    }
  }
}
