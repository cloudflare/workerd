// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The TestController passed to test(), and how test() is called. `workerd test`
// runs the plain objects' test() handlers in the order of their names, then
// the classes', so each `...Check` entrypoint runs after the one it checks.

import * as assert from 'node:assert';
import { inspect } from 'node:util';
import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';

const flags = Cloudflare.compatibilityFlags;
const kInternalClass = Symbol.for('cloudflare:internal-class');

// What JSG gives every resource type's constructor and prototype.
function assertRuntimeClass(value, name) {
  const proto = Object.getPrototypeOf(value);
  const ctor = proto.constructor;
  assert.strictEqual(typeof ctor, 'function');
  assert.deepStrictEqual(Reflect.ownKeys(ctor), [
    'length',
    'name',
    'prototype',
  ]);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(ctor, 'length'), {
    value: 0,
    writable: false,
    enumerable: false,
    configurable: true,
  });
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(ctor, 'name'), {
    value: name,
    writable: false,
    enumerable: false,
    configurable: true,
  });
  assert.strictEqual(Object.getPrototypeOf(ctor), Function.prototype);
  assert.strictEqual(Object.getPrototypeOf(proto), Object.prototype);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(ctor, 'prototype'), {
    value: proto,
    writable: flags.immutable_api_prototypes !== true,
    enumerable: false,
    configurable: false,
  });
  assert.deepStrictEqual(
    Object.getOwnPropertyDescriptor(proto, 'constructor'),
    { value: ctor, writable: true, enumerable: false, configurable: true }
  );

  const keys = Reflect.ownKeys(proto).map(String);
  assert.deepStrictEqual(keys, [
    'constructor',
    ...(flags.set_tostring_tag ? ['Symbol(Symbol.toStringTag)'] : []),
    'Symbol(cloudflare:internal-class)',
    'Symbol(kResourceTypeInspect)',
  ]);
  assert.deepStrictEqual(
    Object.getOwnPropertyDescriptor(proto, kInternalClass),
    {
      value: kInternalClass,
      writable: false,
      enumerable: false,
      configurable: false,
    }
  );
  if (flags.set_tostring_tag) {
    assert.deepStrictEqual(
      Object.getOwnPropertyDescriptor(proto, Symbol.toStringTag),
      { value: name, writable: true, enumerable: false, configurable: true }
    );
  }
  assert.strictEqual(
    Object.prototype.toString.call(value),
    flags.set_tostring_tag ? `[object ${name}]` : '[object Object]'
  );

  // Illegal with or without `new`, and through a subclass's super() call.
  const illegal = { name: 'TypeError', message: 'Illegal constructor' };
  assert.throws(() => new ctor(), illegal);
  assert.throws(() => ctor(), illegal);
  assert.throws(() => Reflect.construct(ctor, []), illegal);
  class Subclass extends ctor {}
  assert.throws(() => new Subclass(), illegal);

  assert.ok(Object.isExtensible(value));
}

// The controllers the handlers below got, in the order they ran.
const controllers = [];

export class EchoObject extends DurableObject {
  echo(value) {
    return value;
  }
}

export const shape = {
  async test(ctrl, env, ctx) {
    controllers.push(ctrl);
    assertRuntimeClass(ctrl, 'TestController');
    assert.deepStrictEqual(Reflect.ownKeys(ctrl), []);
    assert.strictEqual(inspect(ctrl), 'TestController {}');
    assert.strictEqual(JSON.stringify(ctrl), '{}');
    // The class isn't a global.
    assert.strictEqual(globalThis.TestController, undefined);
    assert.ok(!('TestController' in globalThis));

    // Not an RpcTarget or a plain object, so RPC refuses it.
    const stub = env.echo.get(env.echo.idFromName('echo'));
    await assert.rejects(() => stub.echo(ctrl), {
      name: 'DataCloneError',
      message:
        'Could not serialize object of type "TestController". This type ' +
        'does not support serialization.',
    });
  },
};

export const args = {
  test(ctrl, env, ctx) {
    controllers.push(ctrl);
    // Called on the exported object, with exactly the controller, env and ctx.
    assert.strictEqual(this, args);
    assert.strictEqual(arguments.length, 3);
    assert.strictEqual(env.MESSAGE, 'hello');
    assert.strictEqual(typeof ctx.waitUntil, 'function');
    assert.strictEqual(typeof ctx.passThroughOnException, 'function');
    assert.strictEqual(
      Object.getPrototypeOf(ctx).constructor.name,
      'ExecutionContext'
    );
    // A non-promise result passes.
    return 42;
  },
};

// Each call gets its own controller, of the same class.
export const shapeCheck = {
  test() {
    assert.strictEqual(controllers.length, 2);
    assert.notStrictEqual(controllers[0], controllers[1]);
    assert.strictEqual(
      Object.getPrototypeOf(controllers[0]),
      Object.getPrototypeOf(controllers[1])
    );
  },
};

// test() is read from the export only when its test runs, so `swap`'s test,
// which runs first, replaces the function that runs for `swapped`.
export const swap = {
  test() {
    swapped.test = function (ctrl, env, ctx) {
      assert.strictEqual(this, swapped);
      assert.strictEqual(arguments.length, 3);
      assertRuntimeClass(ctrl, 'TestController');
      swapped.ran = 'replacement';
    };
  },
};

export const swapped = {
  test() {
    throw new Error('the original ran');
  },
};

export const swappedCheck = {
  test() {
    assert.strictEqual(swapped.ran, 'replacement');
  },
};

// The export's other handlers are read when it is loaded, but its `test` only
// when its test runs, just before it is called.
const proxyReads = [];
export const proxy = new Proxy(
  {
    test() {
      assert.strictEqual(this, proxy);
      assert.ok(proxyReads.includes('fetch'));
      assert.strictEqual(proxyReads.filter((key) => key === 'test').length, 1);
      assert.strictEqual(proxyReads.at(-1), 'test');
    },
  },
  {
    get(target, key, receiver) {
      proxyReads.push(key);
      return Reflect.get(target, key, receiver);
    },
  }
);

// waitUntil() promises are awaited before the test finishes.
let waited = false;
export const waitUntil = {
  test(ctrl, env, ctx) {
    ctx.waitUntil(
      (async () => {
        await scheduler.wait(10);
        waited = true;
      })()
    );
  },
};

export const waitUntilCheck = {
  test() {
    assert.ok(waited);
  },
};

// A thenable is followed only under unwrap_custom_thenables, as for every
// handler that returns a promise.
let thenCalled = false;
export const thenable = {
  test() {
    return {
      then(resolve) {
        thenCalled = true;
        resolve();
      },
    };
  },
};

export const thenableCheck = {
  test() {
    assert.strictEqual(thenCalled, flags.unwrap_custom_thenables === true);
  },
};

// A class entrypoint's test() is called on an instance, with neither env nor
// ctx.
export class ClassEntrypoint extends WorkerEntrypoint {
  test(ctrl, env, ctx) {
    assert.ok(this instanceof ClassEntrypoint);
    assert.strictEqual(arguments.length, 3);
    assertRuntimeClass(ctrl, 'TestController');
    assert.strictEqual(env, undefined);
    assert.strictEqual(ctx, undefined);
    assert.strictEqual(this.env.MESSAGE, 'hello');
  }
}
