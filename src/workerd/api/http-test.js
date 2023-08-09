// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "node:assert";

let scheduledLastCtrl;

export default {
  async fetch(request, env, ctx) {
    const { pathname } = new URL(request.url);
    if (pathname === "/body-length") {
      return Response.json(Object.fromEntries(request.headers));
    }
    return new Response(null, { status: 404 });
  },

  async scheduled(ctrl, env, ctx) {
    scheduledLastCtrl = ctrl;
    if (ctrl.cron === "* * * * 30") ctrl.noRetry();
  },

  async test(ctrl, env, ctx) {
    // Call `fetch()` with known body length
    {
      const body = new FixedLengthStream(3);
      const writer = body.writable.getWriter();
      void writer.write(new Uint8Array([1, 2, 3]));
      void writer.close();
      const response = await env.SERVICE.fetch("http://placeholder/body-length", {
        method: "POST",
        body: body.readable,
      });
      const headers = new Headers(await response.json());
      assert.strictEqual(headers.get("Content-Length"), "3");
      assert.strictEqual(headers.get("Transfer-Encoding"), null);
    }

    // Check `fetch()` with unknown body length
    {
      const body = new IdentityTransformStream();
      const writer = body.writable.getWriter();
      void writer.write(new Uint8Array([1, 2, 3]));
      void writer.close();
      const response = await env.SERVICE.fetch("http://placeholder/body-length", {
        method: "POST",
        body: body.readable,
      });
      const headers = new Headers(await response.json());
      assert.strictEqual(headers.get("Content-Length"), null);
      assert.strictEqual(headers.get("Transfer-Encoding"), "chunked");
    }

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
