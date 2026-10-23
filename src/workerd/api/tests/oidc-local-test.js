// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strict as assert } from 'node:assert';
import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';

const maximumEscapedAudience = '\0'.repeat(2048);
const token = 'eyJhbGciOiJSUzI1NiJ9.eyJleGVjdXRpb24iOiJsb2NhbCJ9.c2ln';

export class Broker extends WorkerEntrypoint {
  async fetch(request) {
    assert.equal(request.method, 'POST');
    assert.equal(new URL(request.url).pathname, '/token');
    assert.equal(request.headers.get('content-type'), 'application/json');
    const options = await request.json();
    assert.deepEqual(
      Object.keys(options),
      options.audience === undefined ? [] : ['audience']
    );
    switch (options.audience) {
      case 'error':
        return new Response('secret error details', { status: 403 });
      case 'large':
        return new Response('x'.repeat(65537), {
          headers: { 'Content-Type': 'text/plain' },
        });
      case 'invalid':
        return new Response('not a JWT', {
          headers: { 'Content-Type': 'text/plain' },
        });
      case 'wrong-type':
        return new Response(token, {
          headers: { 'Content-Type': 'application/json' },
        });
      case undefined:
        break;
      default:
        assert.ok(
          [
            'https://api.example.com/%22exact',
            maximumEscapedAudience,
            'quote\"\\newline\n雪',
          ].includes(options.audience)
        );
    }
    return new Response(token, { headers: { 'Content-Type': 'text/plain' } });
  }
}

async function check(ctx) {
  assert.equal(await ctx.oidc.getToken(), token);
  assert.equal(
    await ctx.oidc.getToken({ audience: 'https://api.example.com/%22exact' }),
    token
  );
  assert.equal(
    await ctx.oidc.getToken({ audience: 'quote\"\\newline\n雪' }),
    token
  );
  assert.equal(
    await ctx.oidc.getToken({ audience: maximumEscapedAudience }),
    token
  );
  for (const audience of ['error', 'large', 'invalid', 'wrong-type']) {
    await assert.rejects(async () => ctx.oidc.getToken({ audience }));
  }
  await assert.rejects(
    async () => ctx.oidc.getToken({ audience: 'error' }),
    /Local OIDC token issuance failed/
  );
  for (const subject of ['object', 'namespace']) {
    await assert.rejects(async () => ctx.oidc.getToken({ subject }), TypeError);
  }
}

export class LocalObject extends DurableObject {
  async check() {
    await check(this.ctx);
  }
}

export const localOidc = {
  async test(controller, env, ctx) {
    if (!ctx.oidc) return;
    await check(ctx);
    await env.objects.getByName('one').check();
    await env.objects.getByName('two').check();
    assert.equal('broker' in env, false);
  },
};
