// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The shape of `ctx` as JavaScript observes it. The rust-execution-context autogate swaps the C++
// ExecutionContext for the Rust one, so the @all-autogates variant runs these assertions against
// the Rust class, and the other variants against the C++ class.

import {
  deepStrictEqual,
  match,
  rejects,
  strictEqual,
  throws,
} from 'node:assert';
import { inspect } from 'node:util';

const flags = globalThis.Cloudflare.compatibilityFlags;

function descriptors(object) {
  return Object.fromEntries(
    Reflect.ownKeys(object).map((key) => {
      const { value, get, set, ...attributes } =
        Object.getOwnPropertyDescriptor(object, key);
      strictEqual(get, undefined);
      strictEqual(set, undefined);
      return [String(key), { ...attributes, type: typeof value }];
    })
  );
}

const data = (type) => ({
  writable: true,
  enumerable: true,
  configurable: true,
  type,
});

export function checkShape(ctx) {
  const proto = Object.getPrototypeOf(ctx);
  strictEqual(proto, ctx.constructor.prototype);
  strictEqual(Object.getPrototypeOf(proto), Object.prototype);
  strictEqual(ctx.constructor.name, 'ExecutionContext');
  strictEqual(ctx.constructor.length, 0);
  strictEqual(Object.isExtensible(ctx), true);

  // The Python entrypoint helper recognizes ctx by its tag.
  strictEqual(
    Object.prototype.toString.call(ctx),
    flags.set_tostring_tag ? '[object ExecutionContext]' : '[object Object]'
  );

  // Methods live on the prototype, in declaration order.
  const methods = [
    'waitUntil',
    'passThroughOnException',
    ...(flags.allow_irrevocable_stub_storage ? ['restore'] : []),
    'mapVirtualHost',
    'abort',
  ];
  const method = { writable: true, enumerable: true, configurable: true };
  deepStrictEqual(descriptors(proto), {
    ...Object.fromEntries(
      methods.map((name) => [name, { ...method, type: 'function' }])
    ),
    constructor: {
      writable: true,
      enumerable: false,
      configurable: true,
      type: 'function',
    },
    ...(flags.set_tostring_tag
      ? {
          'Symbol(Symbol.toStringTag)': {
            writable: true,
            enumerable: false,
            configurable: true,
            type: 'string',
          },
        }
      : {}),
    'Symbol(cloudflare:internal-class)': {
      writable: false,
      enumerable: false,
      configurable: false,
      type: 'symbol',
    },
    'Symbol(kResourceTypeInspect)': {
      writable: false,
      enumerable: false,
      configurable: true,
      type: 'object',
    },
  });
  // Without spec_compliant_property_attributes, every method's length is 0.
  const lengths = flags.spec_compliant_property_attributes
    ? { waitUntil: 1, restore: 1, mapVirtualHost: 2 }
    : {};
  for (const name of methods) {
    strictEqual(proto[name].name, name);
    strictEqual(proto[name].length, lengths[name] ?? 0, name);
  }

  // Properties are lazy data properties of the instance.
  const own = {
    tracing: data('object'),
    access: data('undefined'),
    ...(flags.enable_version_api ? { version: data('undefined') } : {}),
    cache: data('undefined'),
    props: data('object'),
    ...(flags.enable_ctx_exports ? { exports: data('object') } : {}),
  };
  deepStrictEqual(descriptors(ctx), own);
  deepStrictEqual(Object.keys(ctx), Object.keys(own));
  const forIn = [];
  for (const key in ctx) forIn.push(key);
  deepStrictEqual(forIn, [...Object.keys(own), ...methods]);

  deepStrictEqual(ctx.props, {});
  strictEqual(ctx.props, ctx.props);
  strictEqual(
    Object.prototype.toString.call(ctx.tracing),
    flags.set_tostring_tag ? '[object Tracing]' : '[object Object]'
  );
  if (flags.enable_ctx_exports) {
    strictEqual(typeof ctx.exports.Shape, 'function');
  }

  throws(() => new ctx.constructor(), {
    name: 'TypeError',
    message: 'Illegal constructor',
  });
  throws(() => ctx.constructor(), {
    name: 'TypeError',
    message: 'Illegal constructor',
  });
  for (const name of methods) {
    throws(() => proto[name].call({}), {
      name: 'TypeError',
      message: /^Illegal invocation: function called with incorrect `this`/,
    });
  }
  throws(() => ctx.waitUntil(), {
    name: 'TypeError',
    message:
      "Failed to execute 'waitUntil' on 'ExecutionContext': parameter 1 is not of type 'Promise'.",
  });
  throws(() => ctx.mapVirtualHost(), {
    name: 'TypeError',
    message:
      "Failed to execute 'mapVirtualHost' on 'ExecutionContext': parameter 1 is not of type 'Fetcher'.",
  });
  strictEqual(ctx.waitUntil(1), undefined);
  strictEqual(ctx.waitUntil(Promise.resolve()), undefined);
  strictEqual(ctx.passThroughOnException(), undefined);
}

// The errors ctx.restore() fails with before it calls the entrypoint's [restore](): rejections
// under capture_async_api_throws, synchronous throws otherwise.
export async function checkRestoreErrors(ctx) {
  const cases = [
    [
      () => ctx.restore(42),
      {
        name: 'TypeError',
        message:
          "Failed to execute 'restore' on 'ExecutionContext': parameter 1 is not of type 'Object'.",
      },
    ],
    [
      () => ctx.restore(),
      {
        name: 'TypeError',
        message:
          "Failed to execute 'restore' on 'ExecutionContext': parameter 1 is not of type 'Object'.",
      },
    ],
    [
      () => ctx.restore({ f() {} }),
      { name: 'DataCloneError', message: 'f() {} could not be cloned.' },
    ],
    [
      () => ctx.restore({ a: 1 }),
      {
        name: 'TypeError',
        message:
          'The WorkerEntrypoint or DurableObject class does not implement a [restore]() method.',
      },
    ],
  ];
  for (const [call, error] of cases) {
    if (flags.capture_async_api_throws) {
      let promise;
      try {
        promise = call();
      } catch (e) {
        throw new Error(`threw synchronously: ${e}`);
      }
      await rejects(promise, error);
    } else {
      throws(call, error);
    }
  }
}

// What util.inspect() shows of a ctx with every flag that adds a member.
export function checkInspect(ctx) {
  strictEqual(
    inspect(ctx),
    `ExecutionContext {
  tracing: Tracing {},
  access: undefined,
  version: undefined,
  cache: undefined,
  props: {},
  exports: { Shape: LoopbackServiceStub {} }
}`
  );
  strictEqual(
    inspect(ctx, { depth: 0 }),
    `ExecutionContext {
  tracing: [Tracing],
  access: undefined,
  version: undefined,
  cache: undefined,
  props: {},
  exports: [Object]
}`
  );
}

// How ctx.mapVirtualHost() converts its port, given a Fetcher.
export function checkMapVirtualHost(ctx, fetcher) {
  const host = /^[0-9a-f]{32}\.workers\.alt:80$/;
  match(ctx.mapVirtualHost(fetcher, 80), host);
  // Like any integer argument, the port is truncated.
  match(ctx.mapVirtualHost(fetcher, 80.5), host);
  const notInteger = {
    name: 'TypeError',
    message: 'The value cannot be converted because it is not an integer.',
  };
  throws(() => ctx.mapVirtualHost(fetcher, NaN), notInteger);
  throws(() => ctx.mapVirtualHost(fetcher, 'x'), notInteger);
  throws(() => ctx.mapVirtualHost(fetcher, undefined), notInteger);
  throws(() => ctx.mapVirtualHost(fetcher), {
    name: 'TypeError',
    message:
      "Failed to execute 'mapVirtualHost' on 'ExecutionContext': parameter 2 is not of type 'unsigned short integer'.",
  });
  throws(() => ctx.mapVirtualHost(fetcher, -1), {
    name: 'TypeError',
    message:
      'The value cannot be converted because it is negative and this API expects a positive number.',
  });
  throws(() => ctx.mapVirtualHost(fetcher, 65536), {
    name: 'TypeError',
    message: 'Value out of range. Must be less than or equal to 65535.',
  });
}
