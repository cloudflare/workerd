import { strictEqual } from 'node:assert';

strictEqual(eval('1 + 1'), 2);

export const evalTest = {
  test(_ctrl, env) {
    strictEqual(eval('1 + 1'), 2);

    const fn = new Function('a', 'b', 'return a + b;');
    strictEqual(fn(2, 3), 5);
  },
};
