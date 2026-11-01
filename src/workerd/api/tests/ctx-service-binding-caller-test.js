// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, strictEqual } from 'node:assert';
export const serviceCaller = {
  async test(_controller, env, ctx) {
    const a = {
      kind: 'service-binding',
      accountId: 'local',
      workerId: 'local:a',
    };
    const b = { ...a, workerId: 'local:b' };
    const c = { ...a, workerId: 'local:c' };
    strictEqual(await ctx.identity.caller(), null);
    strictEqual(await env.INGRESS.who(), null);
    for (const binding of [env.B, env.NAMED]) {
      deepStrictEqual(await binding.who(), a);
      deepStrictEqual(
        await (
          await binding.fetch('https://b.example', {
            method: 'POST',
            body: 'read first',
            headers: {
              authorization: 'Bearer invalid',
              signature: 'malformed',
              receiverWorkerId: 'forged',
            },
          })
        ).json(),
        { caller: a, body: 'read first' }
      );
    }
    deepStrictEqual(await env.B.hop(), b);
    const concurrent = await Promise.all([
      env.B.who(),
      env.C.invoke(env.B),
      env.C.hopB(),
      env.B.who(),
    ]);
    deepStrictEqual(concurrent, [
      a,
      { error: 'WorkloadIdentityUnavailableError' },
      c,
      a,
    ]);
    deepStrictEqual((await env.C.invokeFetch(env.B)).caller, {
      error: 'WorkloadIdentityUnavailableError',
    });
    using target = await env.B.target();
    deepStrictEqual(await target.who(), {
      error: 'WorkloadIdentityUnavailableError',
    });
    deepStrictEqual(
      await env.B.callback(async () => {
        try {
          return await ctx.identity.caller();
        } catch (error) {
          return { error: error.name };
        }
      }),
      { error: 'WorkloadIdentityUnavailableError' }
    );
    deepStrictEqual(await env.B.loopback(), {
      error: 'WorkloadIdentityUnavailableError',
    });
    strictEqual(await ctx.identity.caller(), null);
  },
};
