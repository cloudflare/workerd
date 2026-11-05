// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of a wrangler.jsonc with two kv_namespaces: each is its
// own namespace, values persist across a restart, and without
// --experimental the server refuses to start and says why.

import { spawn } from 'node:child_process';
import { mkdtemp, stat, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

const WRANGLER = {
  name: 'kv',
  main: 'index.js',
  compatibility_date: '2026-01-01',
  no_bundle: true,
  kv_namespaces: [
    { binding: 'A', id: 'a' },
    { binding: 'B', id: 'b' },
  ],
};

// /<binding>/<key>: PUT the body, GET, DELETE.
const WORKER = `export default {
  async fetch(request, env) {
    const [, binding, key] = new URL(request.url).pathname.split('/');
    const kv = env[binding];
    if (request.method === 'PUT') {
      await kv.put(key, request.body);
      return new Response(null, { status: 204 });
    }
    if (request.method === 'DELETE') {
      await kv.delete(key);
      return new Response(null, { status: 204 });
    }
    const value = await kv.get(key);
    return new Response(value, { status: value === null ? 404 : 200 });
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

test('kv_namespaces in a wrangler.jsonc', async () => {
  const dir = await mkdtemp(join(env.TEST_TMPDIR ?? tmpdir(), 'kv-'));
  await writeFile(join(dir, 'wrangler.jsonc'), JSON.stringify(WRANGLER));
  await writeFile(join(dir, 'index.js'), WORKER);

  const refused = await serve(dir, []);
  assert.notStrictEqual(refused.code, 0);
  assert.match(refused.stderr, /kv_namespaces.*--experimental/s);

  const big = 'x'.repeat(70_000);
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
  const storage = join(dir, '.wrangler/state/workerd/kv');
  assert.ok((await stat(join(storage, 'KVNamespace'))).isDirectory());

  // Two processes on the same storage, as `wrangler dev` and `wrangler kv
  // key put --local` are: each sees the other's writes at once, misses
  // included, since nothing is cached outside a cluster.
  const [one, two] = [
    await serve(dir, ['--experimental']),
    await serve(dir, ['--experimental']),
  ];
  const at = (s, path) => `http://127.0.0.1:${s.port}${path}`;
  const read = async (s, path) => {
    const r = await fetch(at(s, path));
    return r.status === 404 ? null : await r.text();
  };
  assert.strictEqual(await read(one, '/A/shared'), null);
  await fetch(at(two, '/A/shared'), { method: 'PUT', body: 'from two' });
  assert.strictEqual(await read(one, '/A/shared'), 'from two');
  await fetch(at(two, '/A/shared'), { method: 'DELETE' });
  assert.strictEqual(await read(one, '/A/shared'), null);
  await one.stop();
  await two.stop();
});
