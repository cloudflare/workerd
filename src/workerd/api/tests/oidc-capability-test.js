// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
export const forwarding = {
  async test(_controller, env) {
    const direct = await env.IDENTITY.getToken();
    const forwarded = await env.FORWARDER.exercise(env.IDENTITY);
    for (const value of [
      forwarded.token,
      forwarded.grant,
      forwarded.fetch,
      forwarded.customerFetch,
    ]) {
      assert.equal(value.workerId, 'origin-a');
      assert.equal(value.versionId, 'leaf-a');
      assert.equal(value.namespaceId, 'namespace-a');
    }
    assert.equal(forwarded.customer.id, 'origin-a:namespace-a:customer_x');
    assert.equal(forwarded.customerFetch.label, 'customer_x');
    assert.equal(forwarded.sanitized.label, null);
    await assert.rejects(env.IDENTITY.getIdentity(''), /label/);
    assert.equal(forwarded.sanitized.legacySelector, null);
    const identity = await env.IDENTITY.getIdentity('fixed');
    const relayed = await env.FORWARDER.exerciseIdentity(identity);
    assert.equal(relayed.id, identity.id);
    assert.equal(relayed.fetch.label, 'fixed');
    assert.equal(relayed.fetch.workerId, 'origin-a');
    assert.equal(relayed.fetch.versionId, 'leaf-a');
    assert.equal(relayed.fetch.namespaceId, 'namespace-a');
    assert.equal(typeof identity.getIdentity, 'undefined');
    assert.equal(typeof identity.getToken, 'undefined');
    assert.throws(() => {
      identity.id = 'forged';
    }, TypeError);
    const returned = await env.FORWARDER.select(env.IDENTITY);
    assert.equal(
      (await (await returned.fetch('https://api.example/')).json()).label,
      'returned'
    );
    assert.equal(
      (await env.IDENTITY.getIdentity()).id,
      'origin-a:namespace-a:workload'
    );
    await assert.rejects(env.IDENTITY.getIdentity('x', {}), /optional label/);
    assert.equal(forwarded.token.audience, 'urn:fixed');
    assert.equal(forwarded.fetch.audience, 'urn:fixed');
    assert.equal(forwarded.grant.audience, 'https://auth.example/');
    assert.notEqual(direct.jti, forwarded.token.jti);
    await assert.rejects(env.IDENTITY.getToken(undefined), /no arguments/);
    const tokens = await Promise.all(
      Array.from({ length: 8 }, () => env.IDENTITY.getToken())
    );
    assert.equal(new Set(tokens.map(({ jti }) => jti)).size, 8);
  },
};
