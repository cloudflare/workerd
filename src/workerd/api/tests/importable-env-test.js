import {
  strictEqual,
  deepStrictEqual,
  notStrictEqual,
  ok,
  rejects,
} from 'node:assert';
import { env, withEnv } from 'cloudflare:workers';

import { AsyncLocalStorage } from 'node:async_hooks';
const check = new AsyncLocalStorage();

// The env is populated at the top level scope.
strictEqual(env.FOO, 'BAR');

// Cache exists and is accessible.
ok(env.CACHE);
// But fails when used because we're not in an io-context
await rejects(
  env.CACHE.read('hello', async () => {}),
  {
    message: /Disallowed operation called within global scope./,
  }
);

export const importableEnv = {
  async test(_, argEnv) {
    // Accessing the cache initially at the global scope didn't break anything
    const cached = await argEnv.CACHE.read('hello', async () => {
      return {
        value: 123,
        expiration: Date.now() + 10000,
      };
    });
    strictEqual(cached, 123);

    // They aren't the same objects...
    notStrictEqual(env, argEnv);
    // But have all the same stuff...
    deepStrictEqual(env, argEnv);

    // It is populated inside a request
    strictEqual(env.FOO, 'BAR');

    // And following async operations.
    await scheduler.wait(10);
    strictEqual(env.FOO, 'BAR');

    // Mutations to the env carry through as expected...
    env.BAR = 123;
    const { env: otherEnv } = await import('child');
    strictEqual(otherEnv.FOO, 'BAR');
    strictEqual(otherEnv.BAR, 123);
    deepStrictEqual(argEnv, otherEnv);

    // Using withEnv to replace the env...
    const { env: otherEnv2 } = await withEnv({ BAZ: 1 }, async () => {
      await scheduler.wait(0);
      return import('child2');
    });
    strictEqual(otherEnv2.FOO, undefined);
    strictEqual(otherEnv2.BAZ, 1);

    // Original env is unmodified
    strictEqual(env.BAZ, undefined);

    // Verify that JSRPC calls appropriately see the environment.
    const remote = await argEnv.RPC.rpcTarget(undefined);
    await Promise.all([remote.test(), remote.test2()]);
  },

  get fetch() {
    // It's a weird edge case, yes, but let's make sure that the env is
    // available in getters when extracting the default handlers.
    strictEqual(env.FOO, 'BAR');
    return async () => {};
  },

  // Verifies that the environment is available and correctly propagated
  // in custom events like jsrpc.
  rpcTarget() {
    strictEqual(env.FOO, 'BAR');
    return withEnv({ FOO: 'BAZ' }, () => {
      // Arguably, returned RPC targets should probably automatically capture
      // the current async context and propagate that on subsequent calls,
      // but they currently do not... therefore we have to manually capture
      // the async context and be sure to enter it ourselves below.
      const runInAsyncContext = AsyncLocalStorage.snapshot();
      return {
        test() {
          // This one runs with the modified env
          runInAsyncContext(() => strictEqual(env.FOO, 'BAZ'));
        },
        test2() {
          // This one runs with the original env
          strictEqual(env.FOO, 'BAR');
        },
      };
    });
  },
};
