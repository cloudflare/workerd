import { throws, rejects, strictEqual } from 'node:assert';

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
