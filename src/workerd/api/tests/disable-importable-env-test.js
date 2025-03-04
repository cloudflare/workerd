import { env, withEnv } from 'cloudflare:workers';

// This test runs with the disallow-importable-env flag set, meaning that
// the env imported from cloudflare:workers exists but is not-populated
// with the primary environment. Using withEnv, however, would still work.

import { strictEqual, notDeepStrictEqual } from 'node:assert';

strictEqual(env.FOO, undefined);

export const test = {
  async test(_, argEnv) {
    strictEqual(env.FOO, undefined);
    notDeepStrictEqual(argEnv, env);

    // withEnv still works as expected tho
    const { env: otherEnv2 } = await withEnv({ BAZ: 1 }, async () => {
      await scheduler.wait(0);
      return import('child');
    });
    strictEqual(otherEnv2.FOO, undefined);
    strictEqual(otherEnv2.BAZ, 1);
    strictEqual(argEnv.BAZ, undefined);
  },
};
