import { strictEqual, deepStrictEqual, notStrictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';

// The env is populated at the top level scope.
strictEqual(env.FOO, 'BAR');

export const importableEnv = {
  async test(_, argEnv) {
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
    strictEqual(otherEnv, env);
    deepStrictEqual(argEnv, otherEnv);
  },
};
