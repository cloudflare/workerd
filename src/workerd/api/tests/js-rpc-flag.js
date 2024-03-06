// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

// This class does not extend `DurableObject`, but because we have the `js_rpc` compat flag on,
// it'll still accept RPC.
export class DurableObjectExample {
  constructor() {}

  foo() {
    return 123;
  }

  fetch(req) {
    return new Response(req.method + " " + req.url);
  }
}

export default {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName("foo");
    let obj = env.ns.get(id);
    assert.strictEqual(await obj.foo(), 123);

    // Test that the object has the old helper get() method that wraps fetch(). This is deprecated,
    // but enabled because this test's compat date is before the date when these went away.
    assert.strictEqual(await obj.get("http://foo"), "GET http://foo");
  }
}

