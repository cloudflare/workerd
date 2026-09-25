// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, throws } from 'node:assert';
import { DurableObject, retryable } from 'cloudflare:durable-objects';

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
