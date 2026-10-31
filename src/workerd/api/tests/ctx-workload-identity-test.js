import { strictEqual, deepStrictEqual } from 'node:assert';
export const workload = {
  async test(controller, env, ctx) {
    strictEqual(await ctx.identity.caller(), null); // Non-HTTP event.
    const run = async (path, headers, target = env.RECEIVER) =>
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
      (await run('/', { authorization: 'Bearer caller' }, env.UNAVAILABLE))
        .name,
      'WorkloadIdentityUnavailableError'
    );
    deepStrictEqual(await run('/', {}, env.UNAVAILABLE), {
      identity: null,
      body: 'original',
    });
    strictEqual((await run('/rpc', signed)).identity.id, 'Bearer caller');
    const actor = env.DO.get(env.DO.idFromName('one'));
    const actorResults = await Promise.all(
      ['a', 'b'].map((id) =>
        run('/', { authorization: 'Bearer ' + id, signature: 'test' }, actor)
      )
    );
    deepStrictEqual(
      actorResults.map((r) => r.identity.id),
      ['Bearer a', 'Bearer b']
    );
    deepStrictEqual(
      actorResults.map((r) => r.body),
      ['original', 'original']
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
