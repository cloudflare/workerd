// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, deepStrictEqual } from 'node:assert';
export const workload = {
  async test(controller, env, ctx) {
    strictEqual(await ctx.identity.caller(), null); // Non-HTTP event.
    const run = async (path, headers, target = globalThis) =>
      (
        await target.fetch('https://receiver.example' + path, {
          method: 'POST',
          body: 'original',
          headers,
        })
      ).json();
    deepStrictEqual(await run('/', {}), { identity: null, body: 'original' });
    const signed = { authorization: 'Bearer caller', signature: 'test' };
    const success = await run('/', signed);
    strictEqual(success.identity.id, 'Bearer caller');
    strictEqual(success.identity.body, 'original');
    strictEqual(success.body, 'original');
    for (const path of ['/consumed', '/locked'])
      strictEqual((await run(path, signed)).name, 'InvalidStateError');
    strictEqual(
      (await run('/', { authorization: 'Bearer invalid' })).name,
      'WorkloadIdentityError'
    );
    strictEqual(
      (await run('/', { authorization: 'Bearer outage' })).name,
      'WorkloadIdentityUnavailableError'
    );
    strictEqual(
      (await run('/', { authorization: 'Bearer stalled' })).name,
      'WorkloadIdentityUnavailableError'
    );
    const actor = env.DO.get(env.DO.idFromName('one'));
    strictEqual(
      (await run('/', { authorization: 'Bearer a', signature: 'test' }, actor))
        .name,
      'WorkloadIdentityUnavailableError'
    );
    const concurrent = await Promise.all(
      ['a', 'b'].map((id) => run('/', { authorization: 'Bearer ' + id }))
    );
    deepStrictEqual(
      concurrent.map((r) => r.identity.id),
      ['Bearer a', 'Bearer b']
    );
  },
};
