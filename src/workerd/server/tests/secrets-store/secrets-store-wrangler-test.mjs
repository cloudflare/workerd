// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of a wrangler.jsonc with secrets_store_secrets: a secret's
// value is its WORKERD_SECRET_ variable, a secret whose variable is not set
// is not found, and without --experimental the server refuses to start.

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
    { binding: 'MISSING', store_id: 'demo', secret_name: 'missing' },
  ],
};

// GET /<binding> answers the secret, or the error as a 404.
const WORKER = `export default {
  async fetch(request, env) {
    const secret = env[new URL(request.url).pathname.slice(1)];
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

  const server = await serve(dir, ['--experimental'], {
    WORKERD_SECRET_API_KEY: 'hunter2',
  });
  const get = async (path) => {
    const response = await fetch(`http://127.0.0.1:${server.port}${path}`);
    return [response.status, await response.text()];
  };
  assert.deepStrictEqual(await get('/KEY'), [200, 'hunter2']);
  assert.deepStrictEqual(await get('/MISSING'), [
    404,
    'Secret "missing" not found',
  ]);
  await server.stop();
});
