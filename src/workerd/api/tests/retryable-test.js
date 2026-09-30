// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, throws } from 'node:assert';
import { DurableObject } from 'cloudflare:workers';
import { retryable } from 'cloudflare:durable-objects';

// workerd does not transform decorator syntax, so these tests call the decorator the way a
// bundler's standard-decorator output does.
function methodContext(overrides = {}) {
  return {
    kind: 'method',
    name: 'reset',
    static: false,
    private: false,
    addInitializer() {},
    ...overrides,
  };
}

export const returnsTheDecoratedMethod = {
  test() {
    class Counter extends DurableObject {
      reset(value) {
        return value;
      }
    }
    const method = Counter.prototype.reset;
    strictEqual(retryable(method, methodContext()), method);
    strictEqual(
      retryable(method, methodContext({ name: Symbol('reset') })),
      method
    );
  },
};

// Applies `decorators` innermost first to Class.prototype.reset, installs the result, then
// constructs the class twice, running the initializers as a bundler's constructor does. Returns
// the messages passed to console.warn.
function decorateAndConstruct(Class, decorators, Subclass = Class) {
  const initializers = [];
  const context = methodContext({
    addInitializer(initializer) {
      initializers.push(initializer);
    },
  });
  let method = Class.prototype.reset;
  for (const decorator of decorators.toReversed()) {
    method = decorator(method, context);
  }
  Class.prototype.reset = method;

  const warnings = [];
  const warn = console.warn;
  console.warn = (message) => warnings.push(message);
  try {
    for (let i = 0; i < 2; ++i) {
      const instance = Object.create(Subclass.prototype);
      for (const initializer of initializers) initializer.call(instance);
    }
  } finally {
    console.warn = warn;
  }
  return warnings;
}

const wrap = (method) =>
  function (...args) {
    return method.apply(this, args);
  };

export const warnsOnlyWhenAnotherDecoratorReplacesTheMethod = {
  test() {
    class Outermost extends DurableObject {
      reset() {}
    }
    strictEqual(decorateAndConstruct(Outermost, [retryable, wrap]).length, 0);

    class Wrapped extends DurableObject {
      reset() {}
    }
    const warnings = decorateAndConstruct(Wrapped, [wrap, retryable]);
    strictEqual(warnings.length, 1);
    strictEqual(warnings[0].includes('@retryable on method "reset"'), true);

    // A subclass override leaves the decorated method on the base class's prototype.
    class Base extends DurableObject {
      reset() {}
    }
    class Derived extends Base {
      reset() {}
    }
    strictEqual(decorateAndConstruct(Base, [retryable], Derived).length, 0);

    // A throwing Proxy on the prototype chain does not fail construction.
    class ProxiedBase extends DurableObject {
      reset() {}
    }
    class Proxied extends ProxiedBase {}
    Object.setPrototypeOf(
      Proxied.prototype,
      new Proxy(ProxiedBase.prototype, {
        getOwnPropertyDescriptor() {
          throw new Error('trap');
        },
      })
    );
    strictEqual(
      decorateAndConstruct(ProxiedBase, [wrap, retryable], Proxied).length,
      0
    );
  },
};

export const rejectsInvalidTargets = {
  test() {
    const method = function () {};
    const invalid = [
      [undefined, methodContext()],
      [{}, methodContext()],
      [method, undefined],
      [method, methodContext({ kind: 'field' })],
      [method, methodContext({ kind: 'getter' })],
      [method, methodContext({ kind: 'setter' })],
      [method, methodContext({ kind: 'accessor' })],
      [method, methodContext({ kind: 'class' })],
      [method, methodContext({ kind: 'parameter' })],
      [method, methodContext({ static: true })],
      [method, methodContext({ private: true })],
      [method, methodContext({ static: undefined })],
    ];
    for (const [value, context] of invalid) {
      throws(() => retryable(value, context), TypeError);
    }
  },
};
