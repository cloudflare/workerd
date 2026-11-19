// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of a wrangler.jsonc with secrets_store_secrets: a secret's
// WORKERD_SECRET_ variable overrides it and is never stored, a secret written
// through the admin API survives a restart, and without --experimental the
// server refuses to start.

import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';
import { serve } from '../wrangler-serve.mjs';

const WRANGLER = {
  name: 'secrets',
  main: 'index.js',
  compatibility_date: '2026-01-01',
  no_bundle: true,
  secrets_store_secrets: [
    { binding: 'KEY', store_id: 'demo', secret_name: 'api-key' },
    { binding: 'OTHER', store_id: 'demo', secret_name: 'other' },
  ],
};

// GET /<binding> answers the secret, or the error as a 404; POST /<binding>
// creates it with the request's body.
const WORKER = `export default {
  async fetch(request, env) {
    const secret = env[new URL(request.url).pathname.slice(1)];
    if (request.method === 'POST') {
      const admin = await secret['SecretsStoreSecret::admin_api']();
      return new Response(await admin.create(await request.text()));
    }
    try {
      return new Response(await secret.get());
    } catch (e) {
      return new Response(e.message, { status: 404 });
    }
  },
};
`;

test('secrets_store_secrets in a wrangler.jsonc', async () => {
  const dir = await mkdtemp(join(env.TEST_TMPDIR ?? tmpdir(), 'secrets-'));
  await writeFile(join(dir, 'wrangler.jsonc'), JSON.stringify(WRANGLER));
  await writeFile(join(dir, 'index.js'), WORKER);

  const refused = await serve(dir, []);
  assert.notStrictEqual(refused.code, 0);
  assert.match(refused.stderr, /secrets_store_secrets.*--experimental/s);

  const call = async (server, path, body) => {
    const method = body === undefined ? 'GET' : 'POST';
    const url = `http://127.0.0.1:${server.port}${path}`;
    const response = await fetch(url, { method, body });
    return [response.status, await response.text()];
  };
  let server = await serve(dir, ['--experimental'], {
    WORKERD_SECRET_API_KEY: 'hunter2',
  });
  assert.deepStrictEqual(await call(server, '/KEY'), [200, 'hunter2']);
  assert.deepStrictEqual(await call(server, '/OTHER'), [
    404,
    'Secret "other" not found',
  ]);
  assert.strictEqual((await call(server, '/OTHER', 'stored'))[0], 200);
  assert.deepStrictEqual(await call(server, '/OTHER'), [200, 'stored']);
  await server.stop();

  server = await serve(dir, ['--experimental']);
  assert.deepStrictEqual(await call(server, '/OTHER'), [200, 'stored']);
  assert.deepStrictEqual(await call(server, '/KEY'), [
    404,
    'Secret "api-key" not found',
  ]);
  await server.stop();
});
