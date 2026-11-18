// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of a wrangler.jsonc with secrets_store_secrets: a secret's
// value is its WORKERD_SECRET_ variable, a secret whose variable is not set
// is not found, and without --experimental the server refuses to start.

import { spawn } from 'node:child_process';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

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

const CONTROL_FD = 3;

// Runs workerd on `dir`'s wrangler.jsonc until it listens, or until it
// exits. Answers {port, stop} or {code, stderr}.
function serve(dir, args, vars = {}) {
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'serve',
      join(dir, 'wrangler.jsonc'),
      `--control-fd=${CONTROL_FD}`,
      '--socket-addr=http=127.0.0.1:0',
      ...args,
    ],
    { stdio: ['ignore', 'inherit', 'pipe', 'pipe'], env: { ...env, ...vars } }
  );
  let stderr = '';
  child.stderr.on('data', (d) => {
    stderr += d;
    process.stderr.write(d);
  });
  const exited = new Promise((resolve) => child.once('exit', resolve));
  return new Promise((resolve, reject) => {
    let buffer = '';
    child.stdio[CONTROL_FD].on('data', (d) => {
      buffer += d;
      for (const line of buffer.split('\n').slice(0, -1)) {
        const m = JSON.parse(line);
        if (m.event === 'listen') {
          const stop = () => (child.kill(), exited);
          resolve({ port: m.port, stop });
        }
      }
      buffer = buffer.slice(buffer.lastIndexOf('\n') + 1);
    });
    child.once('error', reject);
    exited.then((code) => resolve({ code, stderr }));
  });
}

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
