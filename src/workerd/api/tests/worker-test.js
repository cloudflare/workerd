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
