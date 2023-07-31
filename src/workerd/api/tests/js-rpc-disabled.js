// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export class DurableObjectExample {
  constructor() {}

  async fetch(request) {
    return new Response("OK");
  }

  foo() {
    // Server side impl of foo.
    // Cannot be called over rpc because we haven't enabled the `js_rpc` feature flag.
    throw new Error("This should not be callable over RPC!");
  }
}

export default {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName("foo");
    let obj = env.ns.get(id);
    // We have `experimental` enabled, but the server side rpc call will fail.
    const expected = "TypeError: The receiving Worker does not allow its methods to be called over RPC.";
    try {
      await obj.foo();
      throw new Error("Didn't throw on server side!");
    } catch(e) {
      if (e != expected) {
        throw new Error(`Expected ${expected} but got ${e}`);
      }
    }

    // End it with a call to the DO.
    return await obj.fetch("http://foo/");
  }
}

