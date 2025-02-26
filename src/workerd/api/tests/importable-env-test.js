import { strictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';

// The env is not populated outside of a request
strictEqual(env.FOO, undefined);

export const importableEnv = {
  async test() {
    // It is populated inside a request
    strictEqual(env.FOO, 'BAR');

    // And following async operations.
    await scheduler.wait(10);
    strictEqual(env.FOO, 'BAR');
  },
};
