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
      forwarded.customer,
      forwarded.customerFetch,
    ]) {
      assert.equal(value.workerId, 'origin-a');
      assert.equal(value.versionId, 'leaf-a');
      assert.equal(value.namespaceId, 'namespace-a');
    }
    assert.equal(forwarded.customer.customerId, 'customer_x');
    assert.equal(forwarded.customerFetch.customerId, 'customer_x');
    assert.equal(forwarded.sanitized.customerId, null);
    await assert.rejects(
      env.IDENTITY.fetchForCustomer('', 'https://api.example/'),
      /customerId/
    );
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
