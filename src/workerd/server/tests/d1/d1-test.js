// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The bundled D1 Worker behind the D1 binding, run once with
// `d1_binding_jsrpc` (RPC) and once without (HTTP). Each run has its own
// database, DB, and OTHER, a second one.

import assert from 'node:assert';
export * from 'd1-miniflare-test';
export * from 'd1-export-test';
import { testD1ApiQueriesHappyPath, testD1Exec } from 'd1-api-test-common';

export const binding = {
  async test(_ctrl, env) {
    await testD1ApiQueriesHappyPath(env.DB);
    await testD1Exec(env.DB);
  },
};

export const batchIsOneTransaction = {
  async test(_ctrl, env) {
    await env.DB.exec('CREATE TABLE t (n INTEGER PRIMARY KEY)');
    const insert = env.DB.prepare('INSERT INTO t VALUES (?)');
    await assert.rejects(
      env.DB.batch([insert.bind(1), insert.bind(2), insert.bind(1)]),
      /^Error: D1_ERROR: UNIQUE constraint failed: t\.n/
    );
    await assert.rejects(
      env.DB.exec('INSERT INTO t VALUES (3);\nINSERT INTO t VALUES (3);'),
      /D1_EXEC_ERROR/
    );
    const count = env.DB.prepare('SELECT count(*) AS n FROM t');
    assert.strictEqual(await count.first('n'), 0);
    await env.DB.batch([insert.bind(1), insert.bind(2)]);
    assert.strictEqual(await count.first('n'), 2);
  },
};

// Statements that are only comments are skipped, as Miniflare skips them.
export const commentOnlyStatement = {
  async test(_ctrl, env) {
    const results = await env.DB.batch([
      env.DB.prepare('-- nothing'),
      env.DB.prepare('/* nothing */ ;'),
      env.DB.prepare('SELECT 1 AS n'),
    ]);
    assert.deepStrictEqual(
      results.map((r) => r.results),
      [[{ n: 1 }]]
    );
    await assert.rejects(
      env.DB.prepare('-- nothing').run(),
      /No SQL statements detected/
    );
  },
};

export const databasesAreSeparate = {
  async test(_ctrl, env) {
    await env.DB.exec('CREATE TABLE mine (x)');
    await assert.rejects(
      env.OTHER.prepare('SELECT * FROM mine').all(),
      /D1_ERROR: no such table: mine/
    );
  },
};

export const values = {
  async test(_ctrl, env) {
    const row = await env.DB.prepare(
      'SELECT ? AS b, ? AS f, ? AS s, ? AS n, ? AS t'
    )
      .bind(new Uint8Array([0, 1, 255]), 1.5, 'é', null, true)
      .first();
    assert.deepStrictEqual(row, {
      b: [0, 1, 255],
      f: 1.5,
      s: 'é',
      n: null,
      t: 1,
    });
    const { meta } = await env.DB.prepare('SELECT 1').run();
    assert.strictEqual(meta.served_by_primary, true);
  },
};

export const sessions = {
  async test(_ctrl, env) {
    const session = env.DB.withSession('first-primary');
    assert.strictEqual(session.getBookmark(), null);
    await session.prepare('SELECT 1').run();
    const first = session.getBookmark();
    assert.strictEqual(typeof first, 'string');
    await session.prepare('SELECT 1').run();
    assert.ok(session.getBookmark() > first);
    // A bookmark from elsewhere is a primary's, so any is satisfied.
    const later = env.DB.withSession(first);
    assert.strictEqual(await later.prepare('SELECT 7 AS n').first('n'), 7);
  },
};

export const refusesTransactionControl = {
  async test(_ctrl, env) {
    await assert.rejects(env.DB.prepare('BEGIN').run(), /D1_ERROR/);
    await assert.rejects(
      env.DB.prepare("ATTACH DATABASE 'x' AS x").run(),
      /D1_ERROR/
    );
  },
};

export const trailingComments = {
  async test(_ctrl, env) {
    const one = env.DB.prepare('SELECT 1 AS n; -- one');
    assert.strictEqual(await one.first('n'), 1);
    await env.DB.exec('CREATE TABLE tc (x); -- note');
    const results = await env.DB.batch([
      env.DB.prepare('/* only */'),
      env.DB.prepare('SELECT 2 AS n'),
    ]);
    assert.deepStrictEqual(
      results.map((r) => r.results),
      [[{ n: 2 }]]
    );
    // Comment markers inside strings and identifiers are not comments.
    const row = await env.DB.prepare(
      `SELECT '--a;' AS "b--", 'it''s /*' AS [c;]; /* end */ ;`
    ).first();
    assert.deepStrictEqual(row, { 'b--': '--a;', 'c;': "it's /*" });
  },
};

// A read reports the rowid the last write left, as D1 does.
export const lastRowIdSurvivesReads = {
  async test(_ctrl, env) {
    await env.DB.exec('CREATE TABLE r (v)');
    const insert = env.DB.prepare('INSERT INTO r VALUES (1)');
    const { meta } = await insert.run();
    const read = await env.DB.prepare('SELECT * FROM r').run();
    assert.strictEqual(read.meta.last_row_id, meta.last_row_id);
  },
};
