import {
  strictEqual,
  ok,
  throws
} from 'node:assert';

export const basics = {
  test(ctx, env) {
    strictEqual(env.unsafe.eval('1'), 1);

    // eval does not capture outer scope.
    let m = 1;
    throws(() => env.unsafe.eval('m'));

    throws(() => env.unsafe.eval(' throw new Error("boom"); ', 'foo'), {
      message: 'boom',
      stack: /at foo/
    });

    // Regular dynamic eval is still not allowed
    throws(() => eval(''));
  }
};

export const newFunction = {
  test(ctx, env) {
    const fn = env.unsafe.newFunction('return m', 'bar', 'm');
    strictEqual(fn.length, 1);
    strictEqual(fn.name, 'bar');
    strictEqual(fn(), undefined);
    strictEqual(fn(1), 1);
    strictEqual(fn(fn), fn);
  }
};

export const newAsyncFunction = {
  async test(ctx, env) {
    const fn = env.unsafe.newAsyncFunction('return await m', 'bar', 'm');
    strictEqual(fn.length, 1);
    strictEqual(fn.name, 'bar');
    strictEqual(await fn(), undefined);
    strictEqual(await fn(1), 1);
    strictEqual(await fn(fn), fn);
    strictEqual(await fn(Promise.resolve(1)), 1);
  }
};

export const newAsyncFunction2 = {
  async test(ctx, env) {
    const fn = env.unsafe.newAsyncFunction('return await arguments[0]');
    strictEqual(fn.length, 0);
    strictEqual(fn.name, 'anonymous');
    strictEqual(await fn(), undefined);
    strictEqual(await fn(1), 1);
    strictEqual(await fn(fn), fn);
    strictEqual(await fn(Promise.resolve(1)), 1);
  }
};
