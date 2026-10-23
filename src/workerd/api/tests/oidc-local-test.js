// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strict as assert } from 'node:assert';
import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';

const maximumEscapedAudience = '\0'.repeat(2048);
const token = 'eyJhbGciOiJSUzI1NiJ9.eyJleGVjdXRpb24iOiJsb2NhbCJ9.c2ln';
const failures = new Map([
  ['bad-request', [400, '', /request is invalid/]],
  ['login', [401, '', /Sign in again/]],
  ['error', [403, '', /access denied/]],
  ['renew-session', [409, '', /Create a new development session/]],
  ['rate', [429, '', /Retry after 60 seconds/]],
  ['sessions-full', [429, 'session_limit', /Wait for an existing session/]],
  ['unconfigured', [503, 'not_configured', /not configured/]],
  ['offline', [503, '', /service is unavailable/]],
  [
    'unknown-error',
    [500, 'secret backend error', /Check the dev server connection/],
  ],
]);

export class Broker extends WorkerEntrypoint {
  async fetch(request) {
    assert.equal(request.method, 'POST');
    assert.equal(new URL(request.url).pathname, '/token');
    assert.equal(request.headers.get('content-type'), 'application/json');
    const options = await request.json();
    assert.deepEqual(
      Object.keys(options),
      options.audience === undefined
        ? ['assignment', 'fresh']
        : ['assignment', 'fresh', 'audience']
    );
    assert.equal(options.assignment, 'local-orders');
    assert.equal(typeof options.fresh, 'boolean');
    if (failures.has(options.audience)) {
      const [status, hint] = failures.get(options.audience);
      return new Response('secret error details', {
        status,
        headers: { 'X-Workers-OIDC-Error': hint },
      });
    }
    switch (options.audience) {
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

async function check(env) {
  assert.equal(await env.IDENTITY.getToken(), token);
  assert.equal(
    await env.IDENTITY.getToken({
      audience: 'https://api.example.com/%22exact',
    }),
    token
  );
  assert.equal(
    await env.IDENTITY.getToken({ audience: 'quote\"\\newline\n雪' }),
    token
  );
  assert.equal(
    await env.IDENTITY.getToken({ audience: maximumEscapedAudience }),
    token
  );
  for (const audience of ['large', 'invalid', 'wrong-type']) {
    await assert.rejects(async () => env.IDENTITY.getToken({ audience }));
  }
  for (const [audience, [, , message]] of failures) {
    await assert.rejects(
      async () => env.IDENTITY.getToken({ audience }),
      (error) => {
        assert.match(error.message, message);
        assert.ok(!error.message.includes('secret'));
        return true;
      }
    );
  }
  for (const subject of ['object', 'namespace']) {
    await assert.rejects(
      async () => env.IDENTITY.getToken({ subject }),
      TypeError
    );
  }
}

export class LocalObject extends DurableObject {
  async check() {
    await check(this.env);
  }
}

export const localOidc = {
  async test(controller, env, ctx) {
    if (!env.IDENTITY) return; // The broker service shares this test module.
    assert.equal('oidc' in ctx, false);
    await check(env);
    await env.objects.getByName('one').check();
    await env.objects.getByName('two').check();
    assert.equal('broker' in env, false);
  },
};
