// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve` of a wrangler.jsonc with two d1_databases: each is its
// own database, rows persist across a restart, and without --experimental
// the server refuses to start and says why.

import { spawn } from 'node:child_process';
import { mkdtemp, stat, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

const WRANGLER = {
  name: 'd1',
  main: 'index.js',
  compatibility_date: '2026-01-01',
  no_bundle: true,
  d1_databases: [
    { binding: 'A', database_name: 'a', database_id: 'a' },
    { binding: 'B', database_name: 'b' },
  ],
};

// POST /<binding> inserts the body; GET /<binding> lists the rows.
const WORKER = `export default {
  async fetch(request, env) {
    const db = env[new URL(request.url).pathname.slice(1)];
    await db.exec('CREATE TABLE IF NOT EXISTS t (v TEXT)');
    if (request.method === 'POST') {
      const v = await request.text();
      const { meta } = await db.prepare('INSERT INTO t VALUES (?)').bind(v).run();
      return Response.json(meta.changes);
    }
    const { results } = await db.prepare('SELECT v FROM t ORDER BY rowid').all();
    return Response.json(results.map((r) => r.v));
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

test('d1_databases in a wrangler.jsonc', async () => {
  const dir = await mkdtemp(join(env.TEST_TMPDIR ?? tmpdir(), 'd1-'));
  await writeFile(join(dir, 'wrangler.jsonc'), JSON.stringify(WRANGLER));
  await writeFile(join(dir, 'index.js'), WORKER);

  const refused = await serve(dir, []);
  assert.notStrictEqual(refused.code, 0);
  assert.match(refused.stderr, /d1_databases.*--experimental/s);

  let server = await serve(dir, ['--experimental']);
  const url = (path) => `http://127.0.0.1:${server.port}${path}`;
  const insert = async (path, body) =>
    assert.strictEqual(
      await (await fetch(url(path), { method: 'POST', body })).json(),
      1
    );
  const rows = async (path) => await (await fetch(url(path))).json();
  await insert('/A', 'one');
  await insert('/A', 'two');
  await insert('/B', 'in b');
  assert.deepStrictEqual(await rows('/A'), ['one', 'two']);
  assert.deepStrictEqual(await rows('/B'), ['in b']);
  await server.stop();

  server = await serve(dir, ['--experimental']);
  assert.deepStrictEqual(await rows('/A'), ['one', 'two']);
  assert.deepStrictEqual(await rows('/B'), ['in b']);
  await server.stop();
  const storage = join(dir, '.wrangler/state/workerd/d1');
  assert.ok((await stat(join(storage, 'D1Database'))).isDirectory());
});
