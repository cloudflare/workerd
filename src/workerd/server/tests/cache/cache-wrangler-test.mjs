// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of a wrangler.jsonc: with --experimental its `caches`
// store entries, which persist across a restart; without it they throw.

import { spawn } from 'node:child_process';
import { mkdtemp, readdir, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

const WRANGLER = {
  name: 'app',
  main: 'index.js',
  compatibility_date: '2026-01-01',
  no_bundle: true,
};

// /<cache>/<key>: PUT the body, GET, DELETE; `default` is caches.default.
const WORKER = `export default {
  async fetch(request) {
    const [, name, key] = new URL(request.url).pathname.split('/');
    const cache = name === 'default' ? caches.default : await caches.open(name);
    const url = 'http://cached/' + key;
    try {
      if (request.method === 'PUT') {
        const headers = { 'Cache-Control': 'max-age=3600' };
        await cache.put(url, new Response(request.body, { headers }));
        return new Response(null, { status: 204 });
      }
      if (request.method === 'DELETE') {
        return new Response(String(await cache.delete(url)));
      }
      return (await cache.match(url)) ?? new Response(null, { status: 404 });
    } catch (e) {
      return new Response(String(e), { status: 500 });
    }
  },
};
`;

const CONTROL_FD = 3;

// Runs workerd on `dir`'s wrangler.jsonc until it listens. Answers
// {port, stop}.
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
    { stdio: ['ignore', 'inherit', 'inherit', 'pipe'] }
  );
  const exited = new Promise((resolve) => child.once('exit', resolve));
  return new Promise((resolve, reject) => {
    let buffer = '';
    child.stdio[CONTROL_FD].on('data', (d) => {
      buffer += d;
      for (const line of buffer.split('\n').slice(0, -1)) {
        const m = JSON.parse(line);
        if (m.event === 'listen') {
          resolve({ port: m.port, stop: () => (child.kill(), exited) });
        }
      }
      buffer = buffer.slice(buffer.lastIndexOf('\n') + 1);
    });
    child.once('error', reject);
    exited.then((code) => reject(new Error(`workerd exited: ${code}`)));
  });
}

test('caches in a wrangler.jsonc', async () => {
  const dir = await mkdtemp(join(env.TEST_TMPDIR ?? tmpdir(), 'cache-'));
  await writeFile(join(dir, 'wrangler.jsonc'), JSON.stringify(WRANGLER));
  await writeFile(join(dir, 'index.js'), WORKER);

  let server = await serve(dir, []);
  const url = (path) => `http://127.0.0.1:${server.port}${path}`;
  const get = async (path) => {
    const r = await fetch(url(path));
    return { status: r.status, text: await r.text() };
  };
  const put = async (path, body) =>
    (await fetch(url(path), { method: 'PUT', body })).status;
  assert.strictEqual(await put('/default/k', 'v'), 500);
  assert.match((await get('/default/k')).text, /No Cache was configured/);
  await server.stop();

  server = await serve(dir, ['--experimental']);
  assert.strictEqual(await put('/default/k', 'in default'), 204);
  assert.strictEqual(await put('/named/k', 'in named'), 204);
  assert.deepStrictEqual(await get('/default/k'), {
    status: 200,
    text: 'in default',
  });
  await server.stop();

  server = await serve(dir, ['--experimental']);
  assert.strictEqual((await get('/default/k')).text, 'in default');
  assert.strictEqual((await get('/named/k')).text, 'in named');
  const del = async (path) =>
    (await fetch(url(path), { method: 'DELETE' })).text();
  assert.strictEqual(await del('/default/k'), 'true');
  assert.strictEqual(await del('/default/k'), 'false');
  assert.strictEqual((await get('/default/k')).status, 404);
  assert.strictEqual((await get('/named/k')).text, 'in named');
  await server.stop();
  const files = await readdir(join(dir, '.wrangler/state/workerd/cache'));
  assert.deepStrictEqual(files.length, 1);
});
