// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Miniflare's D1 tests, ported from workers-sdk ca55031d,
// packages/miniflare/test/plugins/d1/: suite.ts, jsrpc.spec.ts and
// index.spec.ts. Re-exported by d1-test.js, so they run under both
// protocols. The database is shared within a service, so table names are
// unique per test. Miniflare-specific expectations are adapted with a
// comment; real differences are marked DIFF.

import assert from 'node:assert';

const rpc = () => Cloudflare.compatibilityFlags.d1_binding_jsrpc;

let seq = 0;
const unique = (name) => `${name}_${++seq}_${Date.now()}`;

// suite.ts beforeEach: three tables, with colours and one palette.
async function setup(db) {
  const c = unique('colours');
  const k = unique('kitchen_sink');
  const p = unique('palettes');
  // exec() splits on newlines, so one statement per line.
  await db.exec(
    [
      `CREATE TABLE ${c} (id INTEGER PRIMARY KEY, name TEXT NOT NULL, ` +
        'rgb INTEGER NOT NULL);',
      `CREATE TABLE ${k} (id INTEGER PRIMARY KEY, int INTEGER, ` +
        'real REAL, text TEXT, blob BLOB);',
      `CREATE TABLE ${p} (id INTEGER PRIMARY KEY, name TEXT NOT NULL, ` +
        `colour_id INTEGER NOT NULL, FOREIGN KEY (colour_id) ` +
        `REFERENCES ${c}(id));`,
      `INSERT INTO ${c} (id, name, rgb) VALUES (1, 'red', 0xff0000);`,
      `INSERT INTO ${c} (id, name, rgb) VALUES (2, 'green', 0x00ff00);`,
      `INSERT INTO ${c} (id, name, rgb) VALUES (3, 'blue', 0x0000ff);`,
      `INSERT INTO ${p} (id, name, colour_id) VALUES (1, 'Night', 3);`,
    ].join('\n')
  );
  return { c, k, p };
}

// Miniflare reports served_by "miniflare.db"; workerd's D1 "workerd".
const meta = (m, rest) => ({
  served_by: 'workerd',
  served_by_primary: true,
  total_attempts: 1,
  duration: m.duration,
  timings: { sql_duration_ms: m.duration },
  size_after: m.size_after,
  ...rest,
});

const COLOURS = [
  { id: 1, name: 'red', rgb: 0xff0000 },
  { id: 2, name: 'green', rgb: 0x00ff00 },
  { id: 3, name: 'blue', rgb: 0x0000ff },
];

// workerd now returns the last statement's result for a multi-statement
// prepare; upstream also accepts the older error.
async function lastOrRefused(promise, check) {
  try {
    check(await promise);
  } catch (e) {
    if (e instanceof assert.AssertionError) throw e;
    assert.match(e.message, /must contain only one statement/);
  }
}

// suite.ts

export const mfBatch = {
  async test(_ctrl, env) {
    const db = env.DB;
    const { c } = await setup(db);
    const insert = db.prepare(
      `INSERT INTO ${c} (id, name, rgb) VALUES (?, ?, ?)`
    );
    const results = await db.batch([
      insert.bind(4, 'yellow', 0xffff00),
      db.prepare(`SELECT name FROM ${c}`),
    ]);
    assert.strictEqual(results.length, 2);
    assert.strictEqual(results[0].success, true);
    assert.deepStrictEqual(results[0].results, []);
    assert.strictEqual(results[1].success, true);
    const names = ['red', 'green', 'blue', 'yellow'].map((name) => ({ name }));
    assert.deepStrictEqual(results[1].results, names);

    // An error mid-batch rolls back the entire batch. Upstream's throwCause
    // asserts message === "D1_ERROR" and only passes because the resulting
    // AssertionError's text contains "syntax error"; check the real shape.
    const bad = db.prepare(`PUT IN ${c} (id, name, rgb) VALUES (?, ?, ?)`);
    await assert.rejects(
      db.batch([
        insert.bind(5, 'purple', 0xff00ff),
        bad.bind(6, 'blurple', 0x5865f2),
        insert.bind(7, 'cyan', 0x00ffff),
      ]),
      (e) => {
        assert.match(e.message, /^D1_ERROR: .*syntax error/);
        assert.match(e.cause.message, /syntax error/);
        return true;
      }
    );
    const after = await db.prepare(`SELECT name FROM ${c}`).all();
    assert.deepStrictEqual(after.results, names);
  },
};

export const mfExec = {
  async test(_ctrl, env) {
    const db = env.DB;
    const { c } = await setup(db);
    let r = await db.exec(`UPDATE ${c} SET name = 'Red' WHERE name = 'red'`);
    assert.strictEqual(r.count, 1);
    assert.ok(r.duration >= 0);
    let all = await db
      .prepare(`SELECT name FROM ${c} WHERE name = 'Red'`)
      .all();
    assert.deepStrictEqual(all.results, [{ name: 'Red' }]);

    r = await db.exec(
      [
        `UPDATE ${c} SET name = 'Green' WHERE name = 'green'`,
        `UPDATE ${c} SET name = 'Blue' WHERE name = 'blue'`,
      ].join('\n')
    );
    assert.strictEqual(r.count, 2);
    assert.ok(r.duration >= 0);
    all = await db.prepare(`SELECT name FROM ${c}`).all();
    assert.deepStrictEqual(all.results, [
      { name: 'Red' },
      { name: 'Green' },
      { name: 'Blue' },
    ]);
  },
};

export const mfBind = {
  async test(_ctrl, env) {
    const db = env.DB;
    const { c, k } = await setup(db);
    const blob = [...new TextEncoder().encode('Walshy')];
    await db
      .prepare(
        `INSERT INTO ${k} (id, int, real, text, blob) VALUES (?, ?, ?, ?, ?)`
      )
      .bind(1, 42, 3.141, '🙈', blob)
      .run();
    let r = await db.prepare(`SELECT * FROM ${k}`).all();
    assert.deepStrictEqual(r.results, [
      { id: 1, int: 42, real: 3.141, text: '🙈', blob },
    ]);

    await db.prepare(`UPDATE ${k} SET blob = ?`).bind(null).run();
    r = await db.prepare(`SELECT * FROM ${k}`).all();
    assert.deepStrictEqual(r.results, [
      { id: 1, int: 42, real: 3.141, text: '🙈', blob: null },
    ]);

    await assert.rejects(
      db
        .prepare(
          `SELECT * FROM ${c} WHERE name = ?; SELECT * FROM ${c} WHERE id = ?;`
        )
        .bind('green')
        .all(),
      /must contain only one statement|only the last statement can have/
    );

    // Numbered parameters, https://github.com/cloudflare/miniflare/issues/504
    await db
      .prepare(`INSERT INTO ${c} (id, name, rgb) VALUES (?3, ?1, ?2)`)
      .bind('yellow', 0xffff00, 4)
      .run();
    const row = await db
      .prepare(`SELECT * FROM ${c} WHERE id = ?1`)
      .bind(4)
      .first();
    assert.deepStrictEqual(row, { id: 4, name: 'yellow', rgb: 0xffff00 });
  },
};

export const mfFirst = {
  async test(_ctrl, env) {
    const db = env.DB;
    const { c } = await setup(db);
    const select = db.prepare(`SELECT * FROM ${c}`);
    assert.deepStrictEqual(await select.first(), COLOURS[0]);
    assert.strictEqual(await select.first('id'), 1);

    await lastOrRefused(
      db
        .prepare(
          `SELECT * FROM ${c} WHERE name = 'none';
           SELECT * FROM ${c} WHERE id = 1;`
        )
        .first(),
      (r) => assert.deepStrictEqual(r, COLOURS[0])
    );

    const r = await db
      .prepare(`INSERT INTO ${c} (id, name, rgb) VALUES (?, ?, ?)`)
      .bind(4, 'yellow', 0xffff00)
      .first();
    assert.strictEqual(r, null);
    const id = await db
      .prepare(`SELECT id FROM ${c} WHERE name = ?`)
      .bind('yellow')
      .first('id');
    assert.strictEqual(id, 4);
  },
};

export const mfRun = {
  async test(_ctrl, env) {
    const db = env.DB;
    const { c, k } = await setup(db);
    let r = await db.prepare(`SELECT * FROM ${c}`).run();
    assert.ok(r.meta.duration >= 0);
    assert.deepStrictEqual(r, {
      success: true,
      results: COLOURS,
      meta: meta(r.meta, {
        changed_db: false,
        changes: 0,
        last_row_id: r.meta.last_row_id,
        rows_read: 3,
        rows_written: 0,
      }),
    });

    r = await db
      .prepare(`INSERT INTO ${c} (id, name, rgb) VALUES (?, ?, ?) RETURNING *`)
      .bind(4, 'yellow', 0xffff00)
      .run();
    assert.deepStrictEqual(r, {
      success: true,
      results: [{ id: 4, name: 'yellow', rgb: 16776960 }],
      meta: meta(r.meta, {
        changed_db: true,
        changes: 1,
        last_row_id: 4,
        rows_read: 2,
        rows_written: 1,
      }),
    });

    await lastOrRefused(
      db
        .prepare(
          `INSERT INTO ${k} (id) VALUES (1); INSERT INTO ${k} (id) VALUES (2);`
        )
        .run(),
      (r) =>
        assert.deepStrictEqual(r, {
          success: true,
          results: [],
          meta: meta(r.meta, {
            changed_db: true,
            changes: 2,
            last_row_id: r.meta.last_row_id,
            rows_read: 1,
            rows_written: 1,
          }),
        })
    );

    r = await db
      .prepare(`INSERT INTO ${c} (id, name, rgb) VALUES (?, ?, ?)`)
      .bind(5, 'orange', 0xff8000)
      .run();
    assert.deepStrictEqual(r, {
      success: true,
      results: [],
      meta: meta(r.meta, {
        changed_db: true,
        changes: 1,
        last_row_id: 5,
        rows_read: 1,
        rows_written: 1,
      }),
    });
  },
};

export const mfAll = {
  async test(_ctrl, env) {
    const db = env.DB;
    const { c } = await setup(db);
    let r = await db.prepare(`SELECT * FROM ${c}`).all();
    assert.ok(r.meta.duration >= 0);
    assert.deepStrictEqual(r, {
      success: true,
      results: COLOURS,
      meta: meta(r.meta, {
        changed_db: false,
        changes: 0,
        last_row_id: r.meta.last_row_id,
        rows_read: 3,
        rows_written: 0,
      }),
    });

    await lastOrRefused(
      db
        .prepare(
          `SELECT * FROM ${c} WHERE id = 1; SELECT * FROM ${c} WHERE id = 3;`
        )
        .all(),
      (r) =>
        assert.deepStrictEqual(r, {
          success: true,
          results: [COLOURS[2]],
          meta: meta(r.meta, {
            changed_db: false,
            changes: 0,
            last_row_id: r.meta.last_row_id,
            rows_read: 1,
            rows_written: 0,
          }),
        })
    );

    r = await db
      .prepare(`INSERT INTO ${c} (id, name, rgb) VALUES (?, ?, ?)`)
      .bind(4, 'yellow', 0xffff00)
      .all();
    assert.deepStrictEqual(r.results, []);
    assert.strictEqual(r.meta.last_row_id, 4);
    assert.strictEqual(r.meta.changes, 1);
    const id = await db
      .prepare(`SELECT id FROM ${c} WHERE name = ?`)
      .bind('yellow')
      .first('id');
    assert.strictEqual(id, 4);

    r = await db
      .prepare(`INSERT INTO ${c} (id, name, rgb) VALUES (?, ?, ?) RETURNING id`)
      .bind(5, 'orange', 0xff8000)
      .all();
    assert.deepStrictEqual(r.results, [{ id: 5 }]);
    assert.strictEqual(r.meta.last_row_id, 5);
    assert.strictEqual(r.meta.changes, 1);
  },
};

export const mfRaw = {
  async test(_ctrl, env) {
    const db = env.DB;
    const { c } = await setup(db);
    const raw = COLOURS.map(Object.values);
    assert.deepStrictEqual(await db.prepare(`SELECT * FROM ${c}`).raw(), raw);

    await lastOrRefused(
      db
        .prepare(
          `SELECT * FROM ${c} WHERE id = 1; SELECT * FROM ${c} WHERE id = 3;`
        )
        .raw(),
      (r) => assert.deepStrictEqual(r, [raw[2]])
    );

    const r = await db
      .prepare(`INSERT INTO ${c} (id, name, rgb) VALUES (?, ?, ?)`)
      .bind(4, 'yellow', 0xffff00)
      .raw();
    assert.deepStrictEqual(r, []);
    const id = await db
      .prepare(`SELECT id FROM ${c} WHERE name = ?`)
      .bind('yellow')
      .first('id');
    assert.strictEqual(id, 4);

    const abc = unique('abc');
    const cde = unique('cde');
    await db.prepare(`CREATE TABLE ${abc} (a INT, b INT, c INT);`).run();
    await db.prepare(`CREATE TABLE ${cde} (c INT, d INT, e INT);`).run();
    await db.prepare(`INSERT INTO ${abc} VALUES (1,2,3),(4,5,6);`).run();
    await db.prepare(`INSERT INTO ${cde} VALUES (7,8,9),(1,2,3);`).run();
    const rows = await db
      .prepare(`SELECT * FROM ${abc}, ${cde};`)
      .raw({ columnNames: true });
    assert.deepStrictEqual(rows, [
      ['a', 'b', 'c', 'c', 'd', 'e'],
      [1, 2, 3, 7, 8, 9],
      [1, 2, 3, 1, 2, 3],
      [4, 5, 6, 7, 8, 9],
      [4, 5, 6, 1, 2, 3],
    ]);
  },
};

// "operations persist D1 data" is skipped: it restarts Miniflare on a
// persistence directory, which a single workerd process cannot do.

// STRANGE is a binding to the database "my/ Database".
export const mfStrangeDatabaseNames = {
  async test(_ctrl, env) {
    const db = env.STRANGE;
    const { c } = await setup(db);
    await db
      .prepare(`INSERT INTO ${c} (id, name, rgb) VALUES (4, 'pink', 0xff00ff);`)
      .run();
    const r = await db.prepare(`SELECT name FROM ${c} WHERE id = 4`).first();
    assert.deepStrictEqual(r, { name: 'pink' });
  },
};

export const mfRowsAndColumns = {
  async test(_ctrl, env) {
    const db = env.DB;
    const { c, p } = await setup(db);
    const r = await db
      .prepare(
        `SELECT ${c}.name, ${p}.name FROM ${c}
         JOIN ${p} ON ${c}.id = ${p}.colour_id`
      )
      .raw();
    assert.deepStrictEqual(r, [['blue', 'Night']]);
  },
};

// The two dumpSql cases are not ported.

// jsrpc.spec.ts. Upstream runs each case in a fresh Miniflare; here
// tables are unique and EMPTY is a database no other test touches.

export const mfQueryRaw = {
  async test(_ctrl, env) {
    if (!rpc()) return;
    const r = await env.DB.query({
      queries: [{ sql: 'SELECT ? AS answer', params: [42] }],
    });
    assert.deepStrictEqual(r.results[0].data, {
      kind: 'raw',
      columns: ['answer'],
      rows: [[42]],
    });
    const m = r.results[0].meta;
    for (const key of ['duration', 'rows_read', 'rows_written']) {
      assert.strictEqual(typeof m[key], 'number');
    }
  },
};

export const mfLocalExport = {
  async test(_ctrl, env) {
    const t = unique('answers');
    await env.DB.exec(
      `CREATE TABLE ${t} (id INTEGER PRIMARY KEY, answer TEXT)`
    );
    const insert = env.DB.prepare(`INSERT INTO ${t} VALUES (?, ?)`);
    await env.DB.batch([insert.bind(1, 'first'), insert.bind(2, 'second')]);
    const [dump] = await env.DB.prepare('PRAGMA miniflare_d1_export(?,?,?);')
      .bind(0, 0)
      .raw();
    await env.OTHER.exec(dump.join('\n'));
    const restored = await env.OTHER.prepare(
      `SELECT * FROM ${t} ORDER BY id`
    ).all();
    assert.deepStrictEqual(restored.results, [
      { id: 1, answer: 'first' },
      { id: 2, answer: 'second' },
    ]);
  },
};

export const mfQueryMeta = {
  async test(_ctrl, env) {
    const m = rpc()
      ? (
          await env.EMPTY.query({
            queries: [{ sql: 'SELECT ? AS answer', params: [42] }],
          })
        ).results[0].meta
      : (await env.EMPTY.prepare('SELECT ? AS answer').bind(42).all()).meta;
    assert.strictEqual(m.duration, m.timings?.sql_duration_ms);
    assert.deepStrictEqual(
      m,
      meta(m, {
        changes: 0,
        last_row_id: 0,
        changed_db: false,
        rows_read: 0,
        rows_written: 0,
      })
    );
    assert.strictEqual(typeof m.duration, 'number');
    assert.strictEqual(typeof m.size_after, 'number');
  },
};

// DIFF: Miniflare names query()'s error "D1_ERROR"; d1.js leaves it
// "Error". A caller without enhanced_error_serialization (compatibility
// dates before 2026-04-21) gets an unknown name folded into the message,
// which the binding prefixes again: "D1_ERROR: D1_ERROR: ...".
export const mfErrors = {
  async test(_ctrl, env) {
    if (!rpc()) return;
    const sql = 'SELECT * FROM missing_table';
    const errors = [];
    for (const op of [
      () => env.DB.query({ queries: [{ sql }] }),
      () => env.DB.prepare(sql).all(),
      () => env.DB.batch([env.DB.prepare(sql)]),
      () => env.DB.exec(sql),
    ]) {
      try {
        await op();
        errors.push(null);
      } catch (e) {
        errors.push({
          name: e.name,
          message: e.message,
          cause: e.cause && { name: e.cause.name, message: e.cause.message },
        });
      }
    }
    const backend = (e) => {
      assert.strictEqual(e.name, 'Error'); // DIFF
      assert.match(e.message, /^no such table: missing_table/);
    };
    backend(errors[0]);
    for (const [e, prefix] of [
      [errors[1], 'D1_ERROR'],
      [errors[2], 'D1_ERROR'],
      [errors[3], 'D1_EXEC_ERROR'],
    ]) {
      assert.strictEqual(e.name, 'Error');
      assert.match(e.message, new RegExp(`^${prefix}: no such table`));
      backend(e.cause);
    }
  },
};

export const mfQueryBookmarks = {
  async test(_ctrl, env) {
    if (!rpc()) return;
    const t = unique('answers');
    const initial = await env.DB.query({
      queries: [{ sql: `CREATE TABLE ${t} (answer INTEGER)` }],
      bookmark: 'first-primary',
    });
    const written = await env.DB.query({
      queries: [{ sql: `INSERT INTO ${t} VALUES (?)`, params: [42] }],
      bookmark: initial.bookmark,
    });
    const read = await env.DB.query({
      queries: [{ sql: `SELECT answer FROM ${t}` }],
      bookmark: written.bookmark,
    });
    const session = env.DB.withSession(read.bookmark);
    const rows = await session.prepare(`SELECT answer FROM ${t}`).all();
    for (const b of [initial.bookmark, written.bookmark, read.bookmark]) {
      assert.match(b, /.+/);
    }
    assert.deepStrictEqual(read.results[0].data, {
      kind: 'raw',
      columns: ['answer'],
      rows: [[42]],
    });
    assert.strictEqual(rows.success, true);
    assert.deepStrictEqual(rows.results, [{ answer: 42 }]);
    assert.match(session.getBookmark(), /.+/);
    assert.strictEqual(env.DB.withSession('first-primary').getBookmark(), null);
  },
};

export const mfQuerySeparateDatabases = {
  async test(_ctrl, env) {
    if (!rpc()) return;
    const t = unique('answers');
    for (const [db, answer] of [
      [env.DB, 42],
      [env.OTHER, 43],
    ]) {
      await db.query({
        queries: [
          { sql: `CREATE TABLE ${t} (answer INTEGER)` },
          { sql: `INSERT INTO ${t} VALUES (?)`, params: [answer] },
        ],
      });
    }
    for (const [db, answer] of [
      [env.DB, 42],
      [env.OTHER, 43],
    ]) {
      const r = await db.query({
        queries: [{ sql: `SELECT answer FROM ${t}` }],
      });
      assert.deepStrictEqual(r.results[0].data, {
        kind: 'raw',
        columns: ['answer'],
        rows: [[answer]],
      });
    }
  },
};

export const mfQueryRawShapes = {
  async test(_ctrl, env) {
    if (!rpc()) return;
    const r = await env.DB.query({
      queries: [
        { sql: 'SELECT 42 AS answer WHERE 0' },
        { sql: 'SELECT 1 AS answer, 2 AS answer' },
        {
          sql: 'SELECT ? AS bytes, ? AS text, ? AS empty',
          params: [[0, 127, 255], 'hello', null],
        },
      ],
    });
    assert.deepStrictEqual(
      r.results.map(({ data }) => data),
      [
        { kind: 'raw', columns: ['answer'], rows: [] },
        { kind: 'raw', columns: ['answer', 'answer'], rows: [[1, 2]] },
        {
          kind: 'raw',
          columns: ['bytes', 'text', 'empty'],
          rows: [[[0, 127, 255], 'hello', null]],
        },
      ]
    );
  },
};

// The error's name is "Error", not "D1_ERROR"; see mfErrors.
export const mfQueryRejectsEmpty = {
  async test(_ctrl, env) {
    if (!rpc()) return;
    for (const [queries, message] of [
      [[], 'No SQL statements detected.'],
      [[{ sql: '-- no statement' }], 'No SQL statements detected.'],
    ]) {
      await assert.rejects(env.DB.query({ queries }), (e) => {
        assert.strictEqual(e.message, message);
        return true;
      });
    }
  },
};

export const mfQueryRollsBack = {
  async test(_ctrl, env) {
    if (!rpc()) return;
    const t = unique('answers');
    await env.DB.query({
      queries: [{ sql: `CREATE TABLE ${t} (answer INTEGER)` }],
    });
    await assert.rejects(
      env.DB.query({
        queries: [
          { sql: `INSERT INTO ${t} VALUES (42)` },
          { sql: 'INSERT INTO missing_table VALUES (43)' },
        ],
      })
    );
    const r = await env.DB.query({
      queries: [{ sql: `SELECT answer FROM ${t}` }],
    });
    assert.deepStrictEqual(r.results[0].data, {
      kind: 'raw',
      columns: ['answer'],
      rows: [],
    });
  },
};

export const mfConvenienceOverRpc = {
  async test(_ctrl, env) {
    if (!rpc()) return;
    const t = unique('answers');
    const exec = await env.DB.exec(`CREATE TABLE ${t} (answer INTEGER)`);
    assert.strictEqual(exec.count, 1);
    assert.strictEqual(typeof exec.duration, 'number');
    const run = await env.DB.prepare(`INSERT INTO ${t} VALUES (?)`)
      .bind(42)
      .run();
    assert.strictEqual(run.success, true);
    assert.deepStrictEqual(run.results, []);
    assert.strictEqual(run.meta.changes, 1);
    assert.strictEqual(run.meta.rows_written, 1);
    const statement = env.DB.prepare(`SELECT answer FROM ${t}`);
    const all = await statement.all();
    assert.strictEqual(all.success, true);
    assert.deepStrictEqual(all.results, [{ answer: 42 }]);
    assert.strictEqual(await statement.first('answer'), 42);
    assert.deepStrictEqual(await statement.raw({ columnNames: true }), [
      ['answer'],
      [42],
    ]);
    const batch = await env.DB.batch([
      statement,
      env.DB.prepare('SELECT 43 AS answer'),
    ]);
    assert.deepStrictEqual(
      batch.map((b) => [b.success, b.results]),
      [
        [true, [{ answer: 42 }]],
        [true, [{ answer: 43 }]],
      ]
    );
  },
};

// index.spec.ts: both cases (a recoverable SQLite error surfacing as a
// catchable D1 error, and rollback when the bookmark write fails) inject
// the failure through a second SQLite connection to Miniflare's persisted
// file. workerd's Durable Object storage offers no such connection.
