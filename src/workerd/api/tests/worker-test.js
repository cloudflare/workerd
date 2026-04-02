// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { throws, rejects, strictEqual } from 'node:assert';
import { mock } from 'node:test';

// We allow eval and new Function() during startup (global scope evaluation).
strictEqual(eval('1+1'), 2); // should work
const StartupFunction = new Function('return 1+1');
strictEqual(StartupFunction(), 2); // should work
await Promise.resolve();
strictEqual(eval('1+1'), 2); // should still work
const StartupFunction2 = new Function('return 2+2');
strictEqual(StartupFunction2(), 4); // should still work
strictEqual((await import('module-does-eval')).default, 2); // should work

export const testStartupEval = {
  async test() {
    throws(() => eval('console.log("This should not work")'), {
      message: /Code generation from strings disallowed for this context/,
    });

    throws(() => new Function('console.log("This should not work")'), {
      message: /Code generation from strings disallowed for this context/,
    });

    await rejects(() => import('another-module-does-eval'), {
      message: /Code generation from strings disallowed for this context/,
    });

    // eval() with no args or a non-string arg is allowed (V8 returns the value
    // as-is per spec since there is no string to compile).
    strictEqual(eval(), undefined);
    strictEqual(eval(undefined), undefined);

    // new Function() with params and a string body is still blocked.
    throws(() => new Function('a', 'b', 'return a + b'), {
      message: /Code generation from strings disallowed for this context/,
    });

    // new Function() with params and undefined body is also blocked (the body
    // becomes the string "undefined" via ToString).
    throws(() => new Function('a', 'b', undefined), {
      message: /Code generation from strings disallowed for this context/,
    });

    // new Function() with no arguments is allowed (the synthesized source
    // matches the known empty-body, no-parameter pattern).
    const emptyFn = new Function();
    strictEqual(typeof emptyFn, 'function');

    // Extending Function with super() and no arguments is also allowed.
    class Foo extends Function {}
    const foo = new Foo();
    strictEqual(typeof foo, 'function');
  },
};

// Test verifies that explicit resource management is, in fact, enabled.
export const disposeTest = {
  test() {
    const fn = mock.fn();
    {
      using _ = {
        [Symbol.dispose]() {
          fn();
        },
      };
    }
    strictEqual(fn.mock.callCount(), 1);
  },
};

export const asyncDisposeTest = {
  async test() {
    const fn = mock.fn(async () => {});
    {
      await using _ = {
        async [Symbol.asyncDispose]() {
          await fn();
        },
      };
    }
    strictEqual(fn.mock.callCount(), 1);
  },
};
