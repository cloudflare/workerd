// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// `workerd serve --threads=4` of a wrangler.jsonc with several d1_databases:
// rows one thread inserts every other thread selects, inserts through every
// thread at once into one database each land once, and the databases'
// objects live on more than one thread, so not all on the first.
//
// A database is one `D1Database` object, which sets no alarm and keeps
// nothing per thread on disk, so the test finds a database's home by what
// holds it up: a thread kept in JavaScript serves nothing else, so a query
// that another thread takes waits for the spin to end exactly when the
// thread that spins is the database's home.

import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env } from 'node:process';
import { scheduler } from 'node:timers/promises';
import { test } from 'node:test';
import assert from 'node:assert';
import {
  PRELUDE,
  json,
  spin,
  threadsOf,
  withServer,
} from '../kv/threads-harness.mjs';

// Enough databases that, spread by a hash over four threads, they are not
// all on one.
const DATABASES = 8;

const WRANGLER = {
  name: 'd1',
  main: 'index.js',
  compatibility_date: '2026-01-01',
  no_bundle: true,
  d1_databases: Array.from({ length: DATABASES }, (_, i) => ({
    binding: `D${i}`,
    database_name: `database-${i}`,
    database_id: `database-${i}`,
  })),
};

// POST /insert?db=<i>&v=<v> inserts `v` into the database `D<i>`, or into
// every database without `db`; GET /all answers every database's rows, by
// database; GET /ping?db=<i> runs a statement on `D<i>`.
const WORKER = `${PRELUDE}
const each = (db) =>
  db === null ? Array.from({ length: ${DATABASES} }, (_, i) => i) : [Number(db)];
async function route(request, env, url) {
  const db = url.searchParams.get('db');
  if (url.pathname === '/insert') {
    const v = url.searchParams.get('v');
    let changes = 0;
    for (const i of each(db)) {
      const d = env['D' + i];
      await d.exec('CREATE TABLE IF NOT EXISTS t (v TEXT)');
      const { meta } = await d.prepare('INSERT INTO t VALUES (?)').bind(v).run();
      changes += meta.changes;
    }
    return Response.json(changes);
  }
  if (url.pathname === '/ping') {
    const { results } = await env['D' + db].prepare('SELECT 1 AS one').all();
    return Response.json(results[0].one);
  }
  const all = [];
  for (const i of each(db)) {
    const d = env['D' + i];
    await d.exec('CREATE TABLE IF NOT EXISTS t (v TEXT)');
    const { results } = await d.prepare('SELECT v FROM t ORDER BY rowid').all();
    all.push(results.map((r) => r.v));
  }
  return Response.json(all);
}
`;

// How long a thread spins while the databases are queried through the
// others: much longer than a query takes.
const SPIN_MS = 2500;

// The databases whose queries waited for `thread` to end a spin. Each query
// is sent once the spin started, and is taken by another thread, since the
// one that spins takes nothing; one that waited is answered once the spin
// ended, and one that did not long before. Undefined if a query was taken
// by `thread` after all, or answered neither long before the end nor after.
async function heldUpBy(server, thread) {
  const { spun } = await spin(server, thread, SPIN_MS);
  // The head is out: the body's pull, and the spin, come next.
  await scheduler.wait(200);
  const sent = Date.now();
  const answers = await Promise.all(
    Array.from({ length: DATABASES }, async (_, i) => {
      const { front, value } = await json(server, { path: `/ping?db=${i}` });
      assert.strictEqual(value, 1);
      return { front, at: Date.now() };
    })
  );
  const { start, end } = await spun;
  assert(start <= sent, `the spin started at ${start}, after ${sent}`);
  const held = [];
  for (const [i, { front, at }] of answers.entries()) {
    if (front === thread) return undefined;
    if (at >= end) held.push(i);
    else if (at > end - SPIN_MS / 2) return undefined;
  }
  return held;
}

test('d1_databases on every thread', async () => {
  const dir = await mkdtemp(join(env.TEST_TMPDIR ?? tmpdir(), 'd1-threads-'));
  await writeFile(join(dir, 'wrangler.jsonc'), JSON.stringify(WRANGLER));
  await writeFile(join(dir, 'index.js'), WORKER);

  await withServer(dir, async (server) => {
    const fronts = await threadsOf(server);

    // A row in every database through each thread in turn, then all of them
    // through each thread.
    for (const [n, thread] of fronts.entries()) {
      const path = `/insert?v=from-${n}`;
      const { value } = await json(server, { method: 'POST', path, thread });
      assert.strictEqual(value, DATABASES);
    }
    const rows = fronts.map((_, n) => `from-${n}`);
    for (const thread of fronts) {
      const { value } = await json(server, { path: '/all', thread });
      assert.deepStrictEqual(value, Array(DATABASES).fill(rows), thread);
    }

    // Inserts into one database through all threads at once: each row once.
    const ROWS = 40;
    const inserts = await Promise.all(
      Array.from({ length: ROWS }, (_, n) =>
        json(server, { method: 'POST', path: `/insert?db=0&v=at-once-${n}` })
      )
    );
    for (const { value } of inserts) assert.strictEqual(value, 1);
    const served = new Set(inserts.map(({ front }) => front));
    assert(served.size > 1, `the inserts were all served by ${[...served]}`);
    for (const thread of fronts) {
      const { value } = await json(server, { path: '/all?db=0', thread });
      assert.deepStrictEqual(
        value[0].slice(rows.length).sort(),
        Array.from({ length: ROWS }, (_, n) => `at-once-${n}`).sort()
      );
      assert.deepStrictEqual(value[0].slice(0, rows.length), rows);
    }

    // Each database's home: the one thread whose spin holds its queries up.
    const homes = new Map();
    for (const thread of fronts) {
      let held;
      for (let tries = 0; tries < 3 && held === undefined; tries++) {
        held = await heldUpBy(server, thread);
      }
      assert(held !== undefined, `no clear answer while ${thread} spun`);
      for (const i of held) {
        assert(!homes.has(i), `database ${i} waited for two threads`);
        homes.set(i, thread);
      }
    }
    assert.strictEqual(homes.size, DATABASES, `homes: ${[...homes]}`);
    console.log('the databases by home thread:', [...homes].sort());
    const threads = new Set(homes.values());
    assert(
      threads.size > 1,
      `every database is on one thread, ${[...threads]}`
    );
  });
});
