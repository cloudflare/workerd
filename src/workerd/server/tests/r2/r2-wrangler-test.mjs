// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of a wrangler.jsonc with two r2_buckets: each is its own
// bucket, objects persist across a restart, and without --experimental the
// server refuses to start and says why.

import { spawn } from 'node:child_process';
import { mkdtemp, stat, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

const WRANGLER = {
  name: 'r2',
  main: 'index.js',
  compatibility_date: '2026-01-01',
  no_bundle: true,
  r2_buckets: [
    { binding: 'A', bucket_name: 'a' },
    { binding: 'B', bucket_name: 'b' },
  ],
};

// /<binding>/<key>: PUT the body, GET, DELETE.
const WORKER = `export default {
  async fetch(request, env) {
    const [, binding, key] = new URL(request.url).pathname.split('/');
    const bucket = env[binding];
    if (request.method === 'PUT') {
      await bucket.put(key, request.body);
      return new Response(null, { status: 204 });
    }
    if (request.method === 'DELETE') {
      await bucket.delete(key);
      return new Response(null, { status: 204 });
    }
    const object = await bucket.get(key);
    return new Response(object?.body, { status: object ? 200 : 404 });
  },
};
`;

const CONTROL_FD = 3;

// Runs workerd on `dir`'s wrangler.jsonc until it listens, or until it
// exits. Answers {port, stop} or {code, stderr}.
function serve(dir, args) {
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'serve',
      join(dir, 'wrangler.jsonc'),
      `--control-fd=${CONTROL_FD}`,
      '--socket-addr=http=127.0.0.1:0',
      ...args,
    ],
    { stdio: ['ignore', 'inherit', 'pipe', 'pipe'] }
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

test('r2_buckets in a wrangler.jsonc', async () => {
  const dir = await mkdtemp(join(env.TEST_TMPDIR ?? tmpdir(), 'r2-'));
  await writeFile(join(dir, 'wrangler.jsonc'), JSON.stringify(WRANGLER));
  await writeFile(join(dir, 'index.js'), WORKER);

  const refused = await serve(dir, []);
  assert.notStrictEqual(refused.code, 0);
  assert.match(refused.stderr, /r2_buckets.*--experimental/s);

  // Past one 8 MiB chunk.
  const big = 'x'.repeat(9 * 1024 * 1024);
  let server = await serve(dir, ['--experimental']);
  const url = (path) => `http://127.0.0.1:${server.port}${path}`;
  const put = async (path, body) =>
    assert.strictEqual(
      (await fetch(url(path), { method: 'PUT', body })).status,
      204
    );
  const get = async (path) => {
    const r = await fetch(url(path));
    return r.status === 404 ? null : await r.text();
  };
  await put('/A/k', 'in a');
  await put('/B/k', 'in b');
  await put('/A/big', big);
  assert.strictEqual(await get('/A/k'), 'in a');
  assert.strictEqual(await get('/B/k'), 'in b');
  assert.strictEqual(await get('/B/big'), null);
  await server.stop();

  server = await serve(dir, ['--experimental']);
  assert.strictEqual(await get('/A/k'), 'in a');
  assert.strictEqual(await get('/A/big'), big);
  await fetch(url('/A/k'), { method: 'DELETE' });
  assert.strictEqual(await get('/A/k'), null);
  assert.strictEqual(await get('/B/k'), 'in b');
  await server.stop();
  const storage = join(dir, '.wrangler/state/workerd/r2');
  assert.ok((await stat(join(storage, 'objects'))).isDirectory());
});
