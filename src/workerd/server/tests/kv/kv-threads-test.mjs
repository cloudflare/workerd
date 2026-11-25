// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve --threads=4` of a wrangler.jsonc with several
// kv_namespaces: what one thread writes or deletes, values stored inline and
// as blobs alike, every other thread reads, and the namespaces' objects live
// on more than the first thread. A namespace is one `KVNamespace` object,
// and each loop thread keeps the alarms of the objects whose home it is in
// an alarm database of its own: `metadata.sqlite` on the first thread,
// `metadata-<n>.sqlite` on thread n. A large value sets its object's alarm,
// so the database that holds the alarm of a namespace says which thread is
// that namespace's home.

import { mkdtemp, readdir, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { DatabaseSync } from 'node:sqlite';
import { test } from 'node:test';
import assert from 'node:assert';
import {
  PRELUDE,
  json,
  request,
  threadsOf,
  withServer,
} from './threads-harness.mjs';

// Enough namespaces that, spread by a hash over four threads, some are not
// on the first.
const NAMESPACES = 8;

const WRANGLER = {
  name: 'kv',
  main: 'index.js',
  compatibility_date: '2026-01-01',
  no_bundle: true,
  kv_namespaces: Array.from({ length: NAMESPACES }, (_, i) => ({
    binding: `K${i}`,
    id: `namespace-${i}`,
  })),
};

// PUT and DELETE /<i>/<key> write and delete a key of the namespace `K<i>`;
// GET /all?keys=<a>,<b> answers every namespace's values of those keys, by
// namespace, null for a missing one.
const WORKER = `${PRELUDE}
async function route(request, env, url) {
  const [, i, key] = url.pathname.split('/');
  if (request.method === 'PUT') {
    await env['K' + i].put(key, await request.text());
    return new Response(null, { status: 204 });
  }
  if (request.method === 'DELETE') {
    await env['K' + i].delete(key);
    return new Response(null, { status: 204 });
  }
  const keys = url.searchParams.get('keys').split(',');
  const all = [];
  for (let n = 0; n < ${NAMESPACES}; n++) {
    const values = {};
    for (const key of keys) values[key] = await env['K' + n].get(key);
    all.push(values);
  }
  return Response.json(all);
}
`;

// Over `INLINE` (64 KiB): a blob on disk, named by the namespace's row.
const big = (i) => `${i}`.repeat(70_000);
const small = (i) => `small value of ${i}`;

// The rows of `_cf_ALARM` in each alarm database of `directory`, by file.
function alarms(directory, files) {
  const rows = new Map();
  for (const file of files) {
    const db = new DatabaseSync(join(directory, file), { readOnly: true });
    try {
      rows.set(
        file,
        db
          .prepare('SELECT actor_id FROM _cf_ALARM')
          .all()
          .map((r) => r.actor_id)
      );
    } finally {
      db.close();
    }
  }
  return rows;
}

test('kv_namespaces on every thread', async () => {
  const dir = await mkdtemp(join(env.TEST_TMPDIR ?? tmpdir(), 'kv-threads-'));
  await writeFile(join(dir, 'wrangler.jsonc'), JSON.stringify(WRANGLER));
  await writeFile(join(dir, 'index.js'), WORKER);

  await withServer(dir, async (server) => {
    const fronts = await threadsOf(server);
    // Each namespace written through one thread, a different one for each
    // of four namespaces in a row.
    for (let i = 0; i < NAMESPACES; i++) {
      const thread = fronts[i % fronts.length];
      for (const [key, value] of [
        ['small', small(i)],
        ['big', big(i)],
      ]) {
        const put = {
          method: 'PUT',
          path: `/${i}/${key}`,
          body: value,
          thread,
        };
        assert.strictEqual((await request(server, put)).status, 204);
      }
    }
    const expected = Array.from({ length: NAMESPACES }, (_, i) => ({
      small: small(i),
      big: big(i),
    }));
    for (const thread of fronts) {
      const { value } = await json(server, {
        path: '/all?keys=small,big',
        thread,
      });
      assert.deepStrictEqual(value, expected, `read through ${thread}`);
    }

    // Deleted through one thread, missing through every other.
    for (let i = 0; i < NAMESPACES; i++) {
      const thread = fronts[(i + 1) % fronts.length];
      const del = { method: 'DELETE', path: `/${i}/small`, thread };
      assert.strictEqual((await request(server, del)).status, 204);
      expected[i].small = null;
    }
    for (const thread of fronts) {
      const { value } = await json(server, {
        path: '/all?keys=small,big',
        thread,
      });
      assert.deepStrictEqual(value, expected, `read through ${thread}`);
    }
  });

  // Every namespace's object set its alarm for its blob, on its home thread.
  // The objects are named by id: the blobs' directory holds one per
  // namespace.
  const storage = join(dir, '.wrangler/state/workerd/kv');
  const ids = (await readdir(join(storage, 'blobs'))).sort();
  assert.strictEqual(ids.length, NAMESPACES, `blob directories: ${ids}`);
  const namespaceDir = join(storage, 'KVNamespace');
  const files = (await readdir(namespaceDir))
    .filter((file) => /^metadata(-\d+)?\.sqlite$/.test(file))
    .sort();
  const rows = alarms(namespaceDir, files);
  assert.deepStrictEqual(
    [...rows.values()].flat().sort(),
    ids,
    `each namespace's alarm once: ${JSON.stringify([...rows])}`
  );
  console.log('the namespaces by alarm database:', [...rows]);
  const elsewhere = [...rows]
    .filter(([file]) => file !== 'metadata.sqlite')
    .flatMap(([, actors]) => actors);
  assert(
    elsewhere.length > 0,
    `every namespace's alarm is the first thread's: ${JSON.stringify([...rows])}`
  );
});
