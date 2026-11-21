// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Golden vectors for `PRAGMA miniflare_d1_export`: GOLDEN is what the
// unmodified upstream dumpSql answered (Miniflare's D1, workers-sdk
// c46d179e) for FIXTURE, by [noSchema, noData, ...tables].

import assert from 'node:assert';

const FIXTURE = [
  'CREATE TABLE plain (id INTEGER PRIMARY KEY, t TEXT, r REAL, b BLOB, n)',
  'CREATE TABLE "Quoted ""T""" (id INTEGER PRIMARY KEY AUTOINCREMENT, "select" TEXT)',
  'CREATE TABLE child (id INTEGER PRIMARY KEY, p INTEGER REFERENCES plain(id) ON DELETE CASCADE)',
  "INSERT INTO plain VALUES (1, 'it''s', 1.5, X'00ff10', NULL)",
  "INSERT INTO plain VALUES (2, 'line1\nline2\rend', 9e999, X'', 'x')",
  "INSERT INTO plain VALUES (3, '', -9e999, NULL, 42)",
  "INSERT INTO plain VALUES (4, 'a\nb\\nc\rd\\re', 0, NULL, NULL)",
  'INSERT INTO "Quoted ""T""" ("select") VALUES (\'a\'), (\'b\')',
  'INSERT INTO child VALUES (1, 1)',
  'CREATE INDEX plain_t ON plain (t)',
  'CREATE VIEW v AS SELECT id, t FROM plain',
  'CREATE TRIGGER trg AFTER INSERT ON child BEGIN UPDATE plain SET n = 1 WHERE id = new.p; END',
];

const GOLDEN = {
  all: [
    'PRAGMA defer_foreign_keys=TRUE;',
    'CREATE TABLE plain (id INTEGER PRIMARY KEY, t TEXT, r REAL, b BLOB, n);',
    'INSERT INTO "plain" ("id","t","r","b","n") VALUES(1,\'it\'\'s\',1.5,X\'00ff10\',NULL);',
    'INSERT INTO "plain" ("id","t","r","b","n") VALUES(2,replace(replace(\'line1\\nline2\\rend\',\'\\r\',char(13)),\'\\n\',char(10)),9e999,X\'\',\'x\');',
    'INSERT INTO "plain" ("id","t","r","b","n") VALUES(3,\'\',-9e999,NULL,42);',
    'INSERT INTO "plain" ("id","t","r","b","n") VALUES(4,replace(replace(\'a\\012b\\nc\\015d\\re\',\'\\015\',char(13)),\'\\012\',char(10)),0,NULL,NULL);',
    'CREATE TABLE IF NOT EXISTS "Quoted ""T""" (id INTEGER PRIMARY KEY AUTOINCREMENT, "select" TEXT);',
    'INSERT INTO "Quoted ""T""" ("id","select") VALUES(1,\'a\');',
    'INSERT INTO "Quoted ""T""" ("id","select") VALUES(2,\'b\');',
    'CREATE TABLE child (id INTEGER PRIMARY KEY, p INTEGER REFERENCES plain(id) ON DELETE CASCADE);',
    'INSERT INTO "child" ("id","p") VALUES(1,1);',
    'DELETE FROM sqlite_sequence;',
    'INSERT INTO "sqlite_sequence" ("name","seq") VALUES(\'Quoted "T"\',2);',
    'CREATE INDEX plain_t ON plain (t);',
    'CREATE TRIGGER trg AFTER INSERT ON child BEGIN UPDATE plain SET n = 1 WHERE id = new.p; END;',
    'CREATE VIEW v AS SELECT id, t FROM plain;',
  ],
  noSchema: [
    'PRAGMA defer_foreign_keys=TRUE;',
    'INSERT INTO "plain" ("id","t","r","b","n") VALUES(1,\'it\'\'s\',1.5,X\'00ff10\',NULL);',
    'INSERT INTO "plain" ("id","t","r","b","n") VALUES(2,replace(replace(\'line1\\nline2\\rend\',\'\\r\',char(13)),\'\\n\',char(10)),9e999,X\'\',\'x\');',
    'INSERT INTO "plain" ("id","t","r","b","n") VALUES(3,\'\',-9e999,NULL,42);',
    'INSERT INTO "plain" ("id","t","r","b","n") VALUES(4,replace(replace(\'a\\012b\\nc\\015d\\re\',\'\\015\',char(13)),\'\\012\',char(10)),0,NULL,NULL);',
    'INSERT INTO "Quoted ""T""" ("id","select") VALUES(1,\'a\');',
    'INSERT INTO "Quoted ""T""" ("id","select") VALUES(2,\'b\');',
    'INSERT INTO "child" ("id","p") VALUES(1,1);',
    'INSERT INTO "sqlite_sequence" ("name","seq") VALUES(\'Quoted "T"\',2);',
  ],
  noData: [
    'PRAGMA defer_foreign_keys=TRUE;',
    'CREATE TABLE plain (id INTEGER PRIMARY KEY, t TEXT, r REAL, b BLOB, n);',
    'CREATE TABLE IF NOT EXISTS "Quoted ""T""" (id INTEGER PRIMARY KEY AUTOINCREMENT, "select" TEXT);',
    'CREATE TABLE child (id INTEGER PRIMARY KEY, p INTEGER REFERENCES plain(id) ON DELETE CASCADE);',
    'DELETE FROM sqlite_sequence;',
    'CREATE INDEX plain_t ON plain (t);',
    'CREATE TRIGGER trg AFTER INSERT ON child BEGIN UPDATE plain SET n = 1 WHERE id = new.p; END;',
    'CREATE VIEW v AS SELECT id, t FROM plain;',
  ],
  plainOnly: [
    'PRAGMA defer_foreign_keys=TRUE;',
    'CREATE TABLE plain (id INTEGER PRIMARY KEY, t TEXT, r REAL, b BLOB, n);',
    'INSERT INTO "plain" ("id","t","r","b","n") VALUES(1,\'it\'\'s\',1.5,X\'00ff10\',NULL);',
    'INSERT INTO "plain" ("id","t","r","b","n") VALUES(2,replace(replace(\'line1\\nline2\\rend\',\'\\r\',char(13)),\'\\n\',char(10)),9e999,X\'\',\'x\');',
    'INSERT INTO "plain" ("id","t","r","b","n") VALUES(3,\'\',-9e999,NULL,42);',
    'INSERT INTO "plain" ("id","t","r","b","n") VALUES(4,replace(replace(\'a\\012b\\nc\\015d\\re\',\'\\015\',char(13)),\'\\012\',char(10)),0,NULL,NULL);',
  ],
};

const ARGS = {
  all: [0, 0],
  noSchema: [1, 0],
  noData: [0, 1],
  plainOnly: [0, 0, 'plain'],
};

export const exportMatchesUpstream = {
  async test(_ctrl, env) {
    const db = env.GOLDEN;
    await db.batch(FIXTURE.map((s) => db.prepare(s)));
    const pragma = db.prepare('PRAGMA miniflare_d1_export(?,?,?);');
    for (const [name, args] of Object.entries(ARGS)) {
      const [rows] = await pragma.bind(...args).raw();
      assert.deepStrictEqual(rows, GOLDEN[name], name);
    }
    // The dump restores: a second database gets the same rows.
    await env.RESTORE.exec(GOLDEN.all.join('\n'));
    const read = (d) => d.prepare('SELECT * FROM plain ORDER BY id').raw();
    assert.deepStrictEqual(await read(env.RESTORE), await read(db));
    await assert.rejects(
      env.RESTORE.prepare('PRAGMA miniflare_d1_export(?,?,?);').bind(0).raw(),
      /D1_ERROR/
    );
  },
};
