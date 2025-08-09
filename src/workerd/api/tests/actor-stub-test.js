// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// We want to test the behavior of the Durable Object stub now that it uses name interception to
// behave like a JS Proxy object.
import * as assert from 'node:assert';

export class DurableObjectExample {
  constructor() {
    this.check = true;
  }

  async fetch(request) {
    return new Response('OK');
  }

  async foo() {
    // Server side impl of foo.
    return 'foo from remote';
  }

  async throw() {
    throw 1;
  }

  thisCheck() {
    if (this.check !== true) {
      throw new Error('incorrect this within rpc function call');
    }
    return true;
  }
}

async function checkDurableObject(obj) {
  assert.equal(await obj.foo(), 'foo from remote');
  assert.equal(await obj.thisCheck(), true);
  assert.deepStrictEqual(await obj.fetch('http://foo/'), new Response('OK'));
  assert.rejects(obj.throw());
}

export default {
  async test(_request, env, _ctx) {
    // This test verifies we can still use registered methods like `fetch()`, and also confirms that
    // dynamic names (foo() in this case) can also be called.
    //
    // We should still be able to define names on the stub in JS and get the expected result.

    // Check properties of DurableObjectId.
    const id = env.ns.idFromName('foo');
    assert.equal(id.name, 'foo');

    // Check that no two DurableObjectId created via `newUniqueId()` are equal.
    const id1 = env.ns.newUniqueId();
    const id2 = env.ns.newUniqueId();
    assert.equal(!id1.equals(id2), true);

    // Check round tripping of DurableObjectId to string.
    //
    // Note: `name` property is dropped from `env.ns.idFromString(id.toString())`
    assert.equal(env.ns.idFromString(id.toString()).toString(), id.toString());

    // Check properties of DurableObject.
    let obj = env.ns.get(id);
    assert.equal(Object.keys(obj).length, 2);
    assert.equal(obj.name, 'foo');
    assert.equal(obj.id, id);

    // Check that we can call methods on a DurableObject.
    checkDurableObject(obj);

    {
      // Check that DurableObject constructed with `locationHint` is equivalent to `obj`.
      let otherObj = env.ns.get(id, { locationHint: 'wnam' });
      assert.deepStrictEqual(obj, otherObj);
      checkDurableObject(otherObj);
    }

    {
      // Check that DurableObject constructed via `getByName()` is equivalent to `obj`.
      let otherObj = env.ns.getByName('foo');
      assert.deepStrictEqual(obj, otherObj);
      checkDurableObject(otherObj);
    }

    {
      // Check that DurableObject constructed with `locationHint` and `getByName()` is equivalent
      // to `obj`.
      let otherObj = env.ns.getByName('foo', { locationHint: 'wnam' });
      assert.deepStrictEqual(obj, otherObj);
      checkDurableObject(otherObj);
    }

    // Check that methods can be defined on DurableObject and are callable.
    obj.baz = () => {
      return 'called baz';
    };
    assert.equal(Object.keys(obj).length, 3);
    assert.equal(typeof obj.baz, 'function');
    assert.equal(obj.baz(), 'called baz');

    return new Response('OK');
  },
};
