// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of a wrangler.jsonc with ratelimits: a binding allows its
// limit and then refuses, and without --experimental the server refuses to
// start and says why.

import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';
import { serve } from '../wrangler-serve.mjs';

const WRANGLER = {
  name: 'ratelimit',
  main: 'index.js',
  compatibility_date: '2026-01-01',
  no_bundle: true,
  ratelimits: [
    { name: 'LIMITER', namespace_id: '1001', simple: { limit: 2, period: 60 } },
  ],
};

const WORKER = `export default {
  async fetch(request, env) {
    const { success } = await env.LIMITER.limit({ key: 'k' });
    return new Response(null, { status: success ? 200 : 429 });
  },
};
`;

test('ratelimits in a wrangler.jsonc', async () => {
  const dir = await mkdtemp(join(env.TEST_TMPDIR ?? tmpdir(), 'ratelimit-'));
  await writeFile(join(dir, 'wrangler.jsonc'), JSON.stringify(WRANGLER));
  await writeFile(join(dir, 'index.js'), WORKER);

  const refused = await serve(dir, []);
  assert.notStrictEqual(refused.code, 0);
  assert.match(refused.stderr, /ratelimits.*--experimental/s);

  const server = await serve(dir, ['--experimental']);
  // Calls straddling a minute boundary would start a new window.
  const second = Date.now() % 60_000;
  if (second > 55_000) await new Promise((r) => setTimeout(r, 60_000 - second));
  const status = async () =>
    (await fetch(`http://127.0.0.1:${server.port}/`)).status;
  assert.deepStrictEqual(
    [await status(), await status(), await status()],
    [200, 200, 429]
  );
  await server.stop();
});
