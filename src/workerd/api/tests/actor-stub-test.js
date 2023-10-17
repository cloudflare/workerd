// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// We want to test the behavior of the Durable Object stub now that it uses name interception to
// behave like a JS Proxy object.
import * as assert from 'node:assert'

export class DurableObjectExample {
  constructor() {}

  async fetch(request) {
    return new Response("OK");
  }

  foo() {
    // Server side impl of foo.
    return new String("foo from remote");
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
    // TODO(soon): Note that we have only implemented the client-side changes, so `obj.foo()` does
    // not try to call a method called "foo" on the remote. This will need to be updated in the
    // future.
    let expected = "Error: WorkerRpc is unimplemented";
    try {
      let foo = obj.foo();
      if (typeof foo != "exception" && foo != expected) {
        throw foo;
      }
    } catch(e) {
      throw new Error(`Expected ${expected} but got ${e}`);
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

    // TODO(soon): Verify we can't call registered methods over RPC when server side is done.

    // Check the keys again, we should have `baz` now.
    keys = Object.keys(obj);
    if (keys.length != 3 || !(keys.includes("baz"))) {
      throw new Error(`New Durable Object stub had unexpected keys: ${keys}`);
    }

    // End it with a call to the DO.
    return await obj.fetch("http://foo/");
  }
}
