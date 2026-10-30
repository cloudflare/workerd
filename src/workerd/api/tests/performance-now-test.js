// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The observable shape of performance.now() and performance.timeOrigin. Run
// unmodified against the JSG implementation (performance-now-test.wd-test) and
// the TypeScript one (performance-now-ts-test.wd-test), so that drift between
// the two fails one config or the other.
//
// Bindings:
// - specCompliant: spec_compliant_property_attributes is on.
// - perfHooks: enable_nodejs_perf_hooks_module is on, which makes Performance
//   a global that inherits from EventTarget.
// - expectTypeScript: the TypeScript implementation must be installed.
// - unsafe: an unsafeEval binding, for optimizedCalls.

import assert from 'node:assert';

const proto = Object.getPrototypeOf(performance);
const nowDescriptor = Object.getOwnPropertyDescriptor(proto, 'now');
const timeOriginDescriptor = Object.getOwnPropertyDescriptor(
  proto,
  'timeOrigin'
);

// Outside a request there is no IoContext, so the clock reads 0.
assert.strictEqual(performance.now(), 0);
assert.strictEqual(performance.timeOrigin, 0);

function assertIllegalInvocation(fn) {
  assert.throws(fn, {
    name: 'TypeError',
    message: /^Illegal invocation: function called with incorrect `this`/,
  });
}

export const nowShape = {
  test() {
    assert.strictEqual(typeof nowDescriptor.value, 'function');
    assert.strictEqual(nowDescriptor.writable, true);
    assert.strictEqual(nowDescriptor.enumerable, true);
    assert.strictEqual(nowDescriptor.configurable, true);
    assert.strictEqual(Object.hasOwn(performance, 'now'), false);

    const now = nowDescriptor.value;
    assert.strictEqual(now.name, 'now');
    assert.strictEqual(now.length, 0);
    assert.strictEqual(Object.hasOwn(now, 'prototype'), false);
    assert.throws(() => new now(), TypeError);
  },
};

export const timeOriginShape = {
  test(_ctrl, env) {
    const { get, set, enumerable, configurable } = timeOriginDescriptor;
    assert.strictEqual(typeof get, 'function');
    assert.strictEqual(set, undefined);
    assert.strictEqual(enumerable, true);
    assert.strictEqual(configurable, true);
    assert.strictEqual(Object.hasOwn(performance, 'timeOrigin'), false);

    assert.strictEqual(get.name, env.specCompliant ? 'get timeOrigin' : '');
    assert.strictEqual(get.length, 0);
  },
};

export const brandChecks = {
  test() {
    const now = nowDescriptor.value;
    const getTimeOrigin = timeOriginDescriptor.get;
    for (const receiver of [
      undefined,
      null,
      1,
      {},
      proto,
      Object.create(performance),
    ]) {
      assertIllegalInvocation(() => now.call(receiver));
      assertIllegalInvocation(() => getTimeOrigin.call(receiver));
    }
  },
};

// V8 takes the fast-API path only from optimized code, and for a method bound
// to a signature (as JSG methods and the TypeScript natives are) only when it
// knows the receiver's map, which a property load such as `p.now` provides.
// So optimize callers written the way user code is, with monomorphic good
// receivers; the TypeScript member and its call into C++ are inlined into them.
// Bad receivers are checked on separate callers, so that they cannot make the
// good ones polymorphic.
// The config sets --allow-natives-syntax, and the natives are compiled through
// the unsafeEval binding so that their syntax stays out of this module's parse.
const OPTIMIZE = `%PrepareFunctionForOptimization(fn);
  fn(receiver);
  fn(receiver);
  %OptimizeFunctionOnNextCall(fn);
  return fn(receiver);`;

export const optimizedCalls = {
  test(_ctrl, env) {
    const optimize = env.unsafe.newFunction(
      OPTIMIZE,
      'optimize',
      'fn',
      'receiver'
    );
    const readNow = (p) => p.now();
    const readTimeOrigin = (p) => p.timeOrigin;
    assert.ok(optimize(readNow, performance) > 0);
    assert.strictEqual(optimize(readTimeOrigin, performance), 0);
    for (let i = 0; i < 2; i++) {
      assert.ok(readNow(performance) > 0);
      assert.strictEqual(readTimeOrigin(performance), 0);
    }

    // A receiver whose map the signature rejects, reached the same way.
    const borrower = Object.create(performance);
    const readBorrowedNow = (p) => p.now();
    const readBorrowedTimeOrigin = (p) => p.timeOrigin;
    assertIllegalInvocation(() => optimize(readBorrowedNow, borrower));
    assertIllegalInvocation(() => optimize(readBorrowedTimeOrigin, borrower));
    for (let i = 0; i < 2; i++) {
      assertIllegalInvocation(() => readBorrowedNow(borrower));
      assertIllegalInvocation(() => readBorrowedTimeOrigin(borrower));
    }
  },
};

export const values = {
  async test() {
    assert.strictEqual(performance.timeOrigin, 0);

    // The IoContext's clock: 0 outside a request (checked at the top level),
    // the time since the epoch inside one.
    const start = performance.now();
    assert.ok(start > 0, `${start} > 0`);

    await scheduler.wait(10);
    const later = performance.now();
    assert.ok(later > start, `${later} > ${start}`);
  },
};

export const everyRouteSharesTheMembers = {
  async test(_ctrl, env) {
    if (!env.perfHooks) return;
    assert.strictEqual(globalThis.Performance.prototype, proto);

    const perfHooks = await import('node:perf_hooks');
    assert.strictEqual(perfHooks.performance, performance);
    assert.strictEqual(perfHooks.Performance.prototype, proto);
    assert.strictEqual(perfHooks.performance.now, nowDescriptor.value);
    assert.strictEqual(perfHooks.performance.timeOrigin, 0);
  },
};

// Without this the suite would pass against the JSG implementation too.
export const typeScriptIsInstalled = {
  test(_ctrl, env) {
    if (!env.expectTypeScript) return;
    const isNative = (fn) =>
      Function.prototype.toString.call(fn).includes('[native code]');
    assert.strictEqual(isNative(nowDescriptor.value), false);
    assert.strictEqual(isNative(timeOriginDescriptor.get), false);
  },
};
