import { strictEqual, deepStrictEqual } from 'node:assert';
export const workload = {
  async test(controller, env, ctx) {
    strictEqual(await ctx.identity.caller(), null); // Non-HTTP event.
    const run = async (
      path,
      headers,
      target = { fetch: globalThis.fetch.bind(globalThis) }
    ) =>
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
    strictEqual(success.identity.signatureAgent, 'Bearer caller');
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
    strictEqual(
      (await run('/rpc', signed)).identity.signatureAgent,
      'Bearer caller'
    );
    const actor = env.DO.get(env.DO.idFromName('one'));
    const actorResults = await Promise.all(
      ['a', 'b'].map((id) =>
        run('/', { authorization: 'Bearer ' + id, signature: 'test' }, actor)
      )
    );
    deepStrictEqual(
      actorResults.map((r) => r.name),
      ['WorkloadIdentityUnavailableError', 'WorkloadIdentityUnavailableError']
    );
    const concurrent = await Promise.all(
      ['a', 'b'].map((id) => run('/', { authorization: 'Bearer ' + id }))
    );
    deepStrictEqual(
      concurrent.map((r) => r.identity.signatureAgent),
      ['Bearer a', 'Bearer b']
    );
  },
};
