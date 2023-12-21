// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// We want to test the behavior of the Durable Object stub now that it uses name interception to
// behave like a JS Proxy object.
import * as assert from 'node:assert'

export class DurableObjectExample {
  constructor() {
    this.check = true;
  }

  async fetch(request) {
    return new Response("OK");
  }

  async foo() {
    // Server side impl of foo.
    return "foo from remote";
  }

  thisCheck() {
    if (this.check !== true) {
      throw new Error('incorrect this within rpc function call');
    }
    return true;
  }
}

export default {
  async test(ctrl, env, ctx) {
    // This test verifies we can still use registered methods like `fetch()`, and also confirms that
    // dynamic names (foo() in this case) can also be called.
    //
    // We should still be able to define names on the stub in JS and get the expected result.

    let id = env.ns.idFromName("foo");
    let obj = env.ns.get(id);
    // Since we have the flag enabled, we should be able to call foo();
    let expected = "foo from remote";
    try {
      let foo = await obj.foo();
      if (typeof foo != "string" && foo != expected) {
        throw foo;
      }
    } catch(e) {
      throw new Error(`Expected ${expected} but got ${e}`);
    }

    // Let's check to make sure the `this` in the called function is correct
    if ((await obj.thisCheck()) !== true) {
      throw new Error('Checking `this` in the DO stub failed');
    }

    // Let's also look at the keys of our stub.
    let keys = Object.keys(obj);
    if (keys.length != 2) {
      // The keys are `id` and `name`.
      throw new Error(`New Durable Object stub had keys: ${keys}`);
    }

    // Now let's define a method on our object, we should be able to call it.
    obj.baz = ()  => { return "called baz"; };
    if (typeof obj.baz != "function") {
      throw new Error(`baz was not a function: ${obj.baz}`);
    }
    // Make sure the call works right.
    if (obj.baz() != "called baz") {
      throw new Error(`obj.baz() returned unexpected value: ${obj.baz()}`);
    }

    // Check the keys again, we should have `baz` now.
    keys = Object.keys(obj);
    if (keys.length != 3 || !(keys.includes("baz"))) {
      throw new Error(`New Durable Object stub had unexpected keys: ${keys}`);
    }

    // End it with a call to the DO.
    return await obj.fetch("http://foo/");
  }
}
