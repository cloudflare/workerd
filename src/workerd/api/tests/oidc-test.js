// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strict as assert } from 'node:assert';
import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';

const unsupported = /OIDC token issuance is not supported by this runtime/;

async function checkOidc(env, ctx) {
  assert.equal('oidc' in ctx, false);
  if (!env.enabled) {
    assert.equal('IDENTITY' in env, false);
    return;
  }
  const binding = env.IDENTITY;
  for (const options of [
    undefined,
    {},
    { fresh: true },
    { audience: 'urn:example:recipient' },
  ]) {
    await assert.rejects(async () => binding.getToken(options), unsupported);
  }
  for (const options of [
    { subject: 'object' },
    { subject: 'namespace' },
    { identityId: 'forged' },
    { claims: {} },
    { issuer: 'forged' },
    { objectId: 'forged' },
    { fresh: 'true' },
    { fresh: 1 },
    { fresh: null },
    { fresh: undefined },
    { audience: '' },
    { audience: '  ' },
    { audience: 1 },
    { audience: '\ud800' },
    { audience: 'urn:bad:%zz' },
    { audience: 'https://example.com/a b' },
    { audience: 'a'.repeat(2049) },
    { [Symbol('claims')]: {} },
    Object.defineProperty({}, 'claims', { value: {} }),
    [],
    'options',
    1,
  ]) {
    await assert.rejects(async () => binding.getToken(options), TypeError);
  }
}

export class OidcObject extends DurableObject {
  async check() {
    await checkOidc(this.env, this.ctx);
  }
}
export class OidcService extends WorkerEntrypoint {
  async check() {
    await checkOidc(this.env, this.ctx);
  }
}
export const oidc = {
  async test(controller, env, ctx) {
    await checkOidc(env, ctx);
    await env.service.check();
    await env.objects.getByName('one').check();
    await env.objects.getByName('two').check();
  },
};
