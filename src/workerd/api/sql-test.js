// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

async function test(state) {
  const storage = state.storage;
  const sql = storage.sql;
  // Test numeric results
  const resultNumber = [...sql.exec('SELECT 123')];
  assert.equal(resultNumber.length, 1);
  assert.equal(resultNumber[0]['123'], 123);

  // Test raw results
  const resultNumberRaw = [...sql.exec('SELECT 123').raw()];
  assert.equal(resultNumberRaw.length, 1);
  assert.equal(resultNumberRaw[0].length, 1);
  assert.equal(resultNumberRaw[0][0], 123);

  // Test string results
  const resultStr = [...sql.exec("SELECT 'hello'")];
  assert.equal(resultStr.length, 1);
  assert.equal(resultStr[0]["'hello'"], 'hello');

  // Test blob results
  const resultBlob = [...sql.exec("SELECT x'ff' as blob")];
  assert.equal(resultBlob.length, 1);
  const blob = new Uint8Array(resultBlob[0].blob);
  assert.equal(blob.length, 1);
  assert.equal(blob[0], 255);

  {
    // Test binding values
    const result = [...sql.exec('SELECT ?', 456)];
    assert.equal(result.length, 1);
    assert.equal(result[0]['?'], 456);
  }

  {
    // Test multiple binding values
    const result = [...sql.exec('SELECT ? + ?', 123, 456)];
    assert.equal(result.length, 1);
    assert.equal(result[0]['? + ?'], 579);
  }

  {
    // Test multiple rows
    const result = [
      ...sql.exec(
        'SELECT 1 AS value\n' +
          'UNION ALL\n' +
          'SELECT 2 AS value\n' +
          'UNION ALL\n' +
          'SELECT 3 AS value;'
      ),
    ];
    assert.equal(result.length, 3);
    assert.equal(result[0]['value'], 1);
    assert.equal(result[1]['value'], 2);
    assert.equal(result[2]['value'], 3);
  }

  {
    // Test multiple rows, manual iteration with next().
    const cursor = sql.exec(
      'SELECT 1 AS col\n' +
        'UNION ALL\n' +
        'SELECT "foo" AS col\n' +
        'UNION ALL\n' +
        'SELECT 3 AS col;'
    );
    assert.deepEqual(cursor.next(), { done: false, value: { col: 1 } });
    assert.deepEqual(cursor.next(), { done: false, value: { col: 'foo' } });
    assert.deepEqual(cursor.next(), { done: false, value: { col: 3 } });
    assert.deepEqual(cursor.next(), { done: true });
  }

  {
    // Test multiple rows using .toArray()
    const cursor = sql.exec(
      'SELECT 1 AS value\n' +
        'UNION ALL\n' +
        'SELECT "foo" AS value\n' +
        'UNION ALL\n' +
        'SELECT 3 AS value;'
    );
    assert.deepEqual(cursor.toArray(), [
      { value: 1 },
      { value: 'foo' },
      { value: 3 },
    ]);
  }

  {
    // Test one row with .one()
    let cursor = sql.exec('SELECT 123 AS foo, "abc" AS bar');
    assert.deepEqual(cursor.one(), { foo: 123, bar: 'abc' });
    // Cursor has been consumed.
    assert.deepEqual([...cursor], []);

    // Multiple results throws.
    assert.throws(
      () =>
        sql
          .exec(
            'SELECT 1 AS value\n' + 'UNION ALL\n' + 'SELECT "foo" AS value;'
          )
          .one(),
      /Expected exactly one result from SQL query, but got multiple results/
    );

    // No results throws.
    sql.exec('CREATE TABLE IF NOT EXISTS empty (x INTEGER)');
    assert.throws(
      () => sql.exec('SELECT * from empty;').one(),
      /Expected exactly one result from SQL query, but got no results/
    );
    sql.exec('DROP TABLE empty');
  }

  // Test partial query ingestion
  assert.deepEqual(sql.ingest(`SELECT 123; SELECT 456;    `).remainder, '    ');
  assert.deepEqual(sql.ingest(`SELECT 123; SELECT 456;`).remainder, '');
  assert.deepEqual(
    sql.ingest(`SELECT 123; SELECT 456`).remainder,
    ' SELECT 456'
  );
  assert.deepEqual(sql.ingest(`SELECT 123; SELECT 45`).remainder, ' SELECT 45');
  assert.deepEqual(sql.ingest(`SELECT 123; SELECT 4`).remainder, ' SELECT 4');
  assert.deepEqual(sql.ingest(`SELECT 123; SELECT `).remainder, ' SELECT ');
  assert.deepEqual(sql.ingest(`SELECT 123; SELECT`).remainder, ' SELECT');
  assert.deepEqual(sql.ingest(`SELECT 123; SELEC`).remainder, ' SELEC');
  assert.deepEqual(sql.ingest(`SELECT 123; SELE`).remainder, ' SELE');
  assert.deepEqual(sql.ingest(`SELECT 123; SEL`).remainder, ' SEL');
  assert.deepEqual(sql.ingest(`SELECT 123; SE`).remainder, ' SE');
  assert.deepEqual(sql.ingest(`SELECT 123; S`).remainder, ' S');
  assert.deepEqual(sql.ingest(`SELECT 123; `).remainder, ' ');
  assert.deepEqual(sql.ingest(`SELECT 123;`).remainder, '');
  assert.deepEqual(sql.ingest(`SELECT 123`).remainder, 'SELECT 123');
  assert.deepEqual(sql.ingest(`SELECT 12`).remainder, 'SELECT 12');
  assert.deepEqual(sql.ingest(`SELECT 1`).remainder, 'SELECT 1');

  // Exec throws with trailing comments
  assert.throws(
    () => sql.exec('SELECT 123; SELECT 456; -- trailing comment'),
    /SQL code did not contain a statement/
  );
  // Ingest does not
  assert.deepEqual(
    sql.ingest(`SELECT 123; SELECT 456; -- trailing comment`).remainder,
    ' -- trailing comment'
  );

  // Ingest throws if statement looks "complete" but is actually a syntax error:
  assert.throws(
    () => sql.ingest(`SELECT * bunk;`),
    /Error: near "bunk": syntax error at offset/
  );
  assert.throws(
    () => sql.ingest(`INSER INTO xyz VALUES ('a'),('b');`),
    /Error: near "INSER": syntax error/
  );
  assert.throws(
    () => sql.ingest(`INSERT INTO xyz VALUES ('a')('b');`),
    /Error: near "\(": syntax error/
  );

  // Test execution of ingested queries by taking an input of 6 INSERT statements, that all
  // add 6 rows of data, then splitting that into a bunch of chunks, then ingesting them all
  {
    sql.exec(`CREATE TABLE streaming(val TEXT);`);

    // Convert to binary otherwise .split can cause corruption for multi-byte chars
    const inputBytes = new TextEncoder().encode(INSERT_36_ROWS);
    const decoder = new TextDecoder();

    // Use a chunk size 1, 3, 9, 27, 81, ... bytes
    for (let length = 1; length < inputBytes.length; length = length * 3) {
      let totalRowsWritten = 0;
      let totalSqlStatements = 0;
      let buffer = '';
      for (let offset = 0; offset < inputBytes.length; offset += length) {
        // Simulate a single "chunk" arriving
        const chunk = inputBytes.slice(offset, offset + length);

        // Append the new chunk to the existing buffer
        buffer += decoder.decode(chunk, { stream: true });

        // Ingest any complete statements and snip those chars off the buffer
        let result = sql.ingest(buffer);
        buffer = result.remainder;
        totalRowsWritten += result.rowsWritten;
        totalSqlStatements += result.statementCount;

        // Simulate awaiting next chunk
        await scheduler.wait(1);
      }

      // Verify exactly 36 rows were added
      assert.deepEqual(Array.from(sql.exec(`SELECT count(*) FROM streaming`)), [
        { 'count(*)': 36 },
      ]);

      // Ensure our precious emoji were preserved, even if their bytes occur across split points
      assert.deepEqual(
        Array.from(sql.exec(`SELECT * FROM streaming WHERE val LIKE 'f%'`)),
        [
          { val: 'f: 😳' },
          { val: 'f: 🫠' },
          { val: 'f: 🙃' },
          { val: 'f: 🤡' },
          { val: 'f: 🥺' },
          { val: 'f: 🔥😎🔥' },
        ]
      );

      // Verify that all 36 rows we inserted were accounted for.
      assert.equal(totalRowsWritten, 36);
      assert.equal(totalSqlStatements, 6);

      sql.exec(`DELETE FROM streaming`);
      await scheduler.wait(1);
    }
    sql.exec(`DROP TABLE streaming;`);
  }

  // Test count
  {
    const result = [
      ...sql.exec(
        'SELECT count(value) from (SELECT 1 AS value\n' +
          'UNION ALL\n' +
          'SELECT 2 AS value\n' +
          'UNION ALL\n' +
          'SELECT 3 AS value);'
      ),
    ];
    assert.equal(result.length, 1);
    assert.equal(result[0]['count(value)'], 3);
  }

  // Test sum
  {
    const result = [
      ...sql.exec(
        'SELECT sum(value) from (SELECT 1 AS value\n' +
          'UNION ALL\n' +
          'SELECT 2 AS value\n' +
          'UNION ALL\n' +
          'SELECT 3 AS value);'
      ),
    ];
    assert.equal(result.length, 1);
    assert.equal(result[0]['sum(value)'], 6);
  }

  // Test math functions enabled
  {
    const result = [...sql.exec('SELECT cos(0)')];
    assert.equal(result.length, 1);
    assert.equal(result[0]['cos(0)'], 1);
  }

  // Empty statements
  assert.throws(() => sql.exec(''), 'SQL code did not contain a statement');
  assert.throws(() => sql.exec(';'), 'SQL code did not contain a statement');

  // Invalid statements
  assert.throws(() => sql.exec('SELECT ;'), /syntax error at offset 7/);
  assert.throws(() => sql.exec('SELECT -;'), /syntax error at offset 8/);

  // Data type mismatch
  sql.exec(`CREATE TABLE test_error_codes (name TEXT);`);
  assert.throws(
    () =>
      sql.exec(
        `INSERT INTO test_error_codes(rowid, name) values ('yeah','nah');`
      ),
    /Error: datatype mismatch: SQLITE_MISMATCH/
  );
  sql.exec(`DROP TABLE test_error_codes;`);

  // Incorrect number of binding values
  assert.throws(
    () => sql.exec('SELECT ?'),
    'Error: Wrong number of parameter bindings for SQL query.'
  );

  // Prepared statement
  const prepared = sql.prepare('SELECT 789');
  const resultPrepared = [...prepared()];
  assert.equal(resultPrepared.length, 1);
  assert.equal(resultPrepared[0]['789'], 789);

  // Running the same query twice, overlapping, works just fine.
  let result1 = prepared();
  let result2 = prepared();
  // Iterate result2 before result1.
  assert.equal([...result2][0]['789'], 789);
  assert.equal([...result1][0]['789'], 789);

  // That said if a cursor was already done before the statement was re-run, it's not considered
  // canceled.
  prepared();
  assert.equal([...result2].length, 0);

  // Prepared statement with binding values
  const preparedWithBinding = sql.prepare('SELECT ?');
  const resultPreparedWithBinding = [...preparedWithBinding(789)];
  assert.equal(resultPreparedWithBinding.length, 1);
  assert.equal(resultPreparedWithBinding[0]['?'], 789);

  // Prepared statement (incorrect number of binding values)
  assert.throws(
    () => preparedWithBinding(),
    'Error: Wrong number of parameter bindings for SQL query.'
  );

  // Prepared statement with whitespace
  const whitespace = [' ', '\t', '\n', '\r', '\v', '\f', '\r\n'];

  for (const char of whitespace) {
    const prepared = sql.prepare(`SELECT 1;${char}`);
    const result = [...prepared()];

    assert.equal(result.length, 1);
  }

  // Prepared statement with multiple statements
  assert.deepEqual([...sql.prepare('SELECT 1; SELECT 2;')()], [{ 2: 2 }]);

  // Accessing a hidden _cf_ table
  assert.throws(
    () => sql.exec('CREATE TABLE _cf_invalid (name TEXT)'),
    /not authorized/
  );
  storage.put('blah', 123); // force creation of _cf_KV table
  assert.throws(
    () => sql.exec('SELECT * FROM _cf_KV'),
    /access to _cf_KV.key is prohibited/
  );

  // Some pragmas are completely not allowed
  assert.throws(
    () => sql.exec('PRAGMA hard_heap_limit = 1024'),
    /not authorized/
  );

  // Test reading read-only pragmas
  {
    const result = [...sql.exec('pragma data_version;')];
    assert.equal(result.length, 1);
    assert.equal(result[0]['data_version'], 2);
  }

  // Trying to write to read-only pragmas is not allowed
  assert.throws(
    () => sql.exec('PRAGMA data_version = 5'),
    /not authorized: SQLITE_AUTH/
  );
  assert.throws(
    () => sql.exec('PRAGMA max_page_count = 65536'),
    /not authorized/
  );
  assert.throws(
    () => sql.exec('PRAGMA page_size = 8192'),
    /not authorized: SQLITE_AUTH/
  );

  // PRAGMA table_info and PRAGMA table_xinfo are allowed.
  sql.exec('CREATE TABLE myTable (foo TEXT, bar INTEGER)');
  {
    let info = [...sql.exec('PRAGMA table_info(myTable)')];
    assert.equal(info.length, 2);
    assert.equal(info[0].name, 'foo');
    assert.equal(info[1].name, 'bar');

    let xInfo = [...sql.exec('PRAGMA table_xinfo(myTable)')];
    assert.equal(xInfo.length, 2);
    assert.equal(xInfo[0].name, 'foo');
    assert.equal(xInfo[1].name, 'bar');
  }

  // Can't get table_info for _cf_KV.
  assert.throws(() => sql.exec('PRAGMA table_info(_cf_KV)'), /not authorized/);

  // Testing the three valid types of inputs for quick_check
  assert.deepEqual(Array.from(sql.exec('pragma quick_check;')), [
    { quick_check: 'ok' },
  ]);
  assert.deepEqual(Array.from(sql.exec('pragma quick_check(1);')), [
    { quick_check: 'ok' },
  ]);
  assert.deepEqual(Array.from(sql.exec('pragma quick_check(100);')), [
    { quick_check: 'ok' },
  ]);
  assert.deepEqual(Array.from(sql.exec('pragma quick_check(myTable);')), [
    { quick_check: 'ok' },
  ]);
  // But that private tables are again restricted
  assert.throws(() => sql.exec('PRAGMA quick_check(_cf_KV)'), /not authorized/);

  // Basic functions like abs() work.
  assert.equal([...sql.exec('SELECT abs(-123)').raw()][0][0], 123);

  // We don't permit sqlite_*() functions.
  assert.throws(
    () => sql.exec('SELECT sqlite_version()'),
    /not authorized to use function: sqlite_version/
  );

  // JSON -> operator works
  const jsonResult = [
    ...sql.exec('SELECT \'{"a":2,"c":[4,5,{"f":7}]}\' -> \'$.c\' AS value'),
  ][0].value;
  assert.equal(jsonResult, '[4,5,{"f":7}]');

  // current_{date,time,timestamp} functions work
  const resultDate = [...sql.exec('SELECT current_date')];
  assert.equal(resultDate.length, 1);
  // Should match results in the format "2023-06-01"
  assert.match(resultDate[0]['current_date'], /^\d{4}-\d{2}-\d{2}$/);

  const resultTime = [...sql.exec('SELECT current_time')];
  assert.equal(resultTime.length, 1);
  // Should match results in the format "15:30:03"
  assert.match(resultTime[0]['current_time'], /^\d{2}:\d{2}:\d{2}$/);

  const resultTimestamp = [...sql.exec('SELECT current_timestamp')];
  assert.equal(resultTimestamp.length, 1);
  // Should match results in the format "2023-06-01 15:30:03"
  assert.match(
    resultTimestamp[0]['current_timestamp'],
    /^\d{4}-\d{2}-\d{2}\s{1}\d{2}:\d{2}:\d{2}$/
  );

  // Validate that the SQLITE_LIMIT_COMPOUND_SELECT limit is enforced as expected
  const compoundWithinLimits = [
    ...sql.exec(
      'SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5'
    ),
  ];
  assert.equal(compoundWithinLimits.length, 5);
  assert.throws(
    () =>
      sql.exec(
        'SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6'
      ),
    /too many terms in compound SELECT/
  );

  // Can't start transactions or savepoints.
  assert.throws(
    () => sql.exec('BEGIN TRANSACTION'),
    /please use the state.storage.transaction\(\) or state.storage.transactionSync\(\) APIs/
  );
  assert.throws(
    () => sql.exec('SAVEPOINT foo'),
    /please use the state.storage.transaction\(\) or state.storage.transactionSync\(\) APIs/
  );

  // Virtual tables
  // Only fts5 and fts5vocab modules are allowed
  assert.throws(
    () => sql.exec(`CREATE VIRTUAL TABLE test_fts USING fts5abcd(id);`),
    /not authorized/
  );

  // Full text search extension
  sql.exec(`
    CREATE TABLE documents (
      id INTEGER PRIMARY KEY,
      title TEXT NOT NULL,
      content TEXT NOT NULL
    );
  `);

  // Module names are case-insensitive
  sql.exec(`
    CREATE VIRTUAL TABLE documents_fts USING FtS5(id, title, content, tokenize = porter);
  `);
  sql.exec(`
    CREATE VIRTUAL TABLE documents_fts_v_col USING fTs5VoCaB(documents_fts, col);
  `);
  sql.exec(`
    CREATE VIRTUAL TABLE documents_fts_v_row USING FtS5vOcAb(documents_fts, row);
  `);
  sql.exec(`
    CREATE VIRTUAL TABLE documents_fts_v_instance USING fTs5VoCaB(documents_fts, instance);
  `);

  sql.exec(`
    CREATE TRIGGER documents_fts_insert
    AFTER INSERT ON documents
    BEGIN
      INSERT INTO documents_fts(id, title, content)
        VALUES(new.id, new.title, new.content);
    END;
  `);
  sql.exec(`
    CREATE TRIGGER documents_fts_update
    AFTER UPDATE ON documents
    BEGIN
      UPDATE documents_fts SET title=new.title, content=new.content WHERE id=old.id;
    END;
  `);
  sql.exec(`
    CREATE TRIGGER documents_fts_delete
    AFTER DELETE ON documents
    BEGIN
      DELETE FROM documents_fts WHERE id=old.id;
    END;
  `);
  sql.exec(`
    INSERT INTO documents (title, content) VALUES ('Document 1', 'This is the contents of document 1 (of 2).');
  `);
  sql.exec(`
    INSERT INTO documents (title, content) VALUES ('Document 2', 'This is the content of document 2 (of 2).');
  `);
  // Porter stemming makes 'contents' and 'content' the same
  {
    let results = Array.from(
      sql.exec(`
      SELECT * FROM documents_fts WHERE documents_fts MATCH 'content' ORDER BY rank;
    `)
    );
    assert.equal(results.length, 2);
    assert.equal(results[0].id, 1); // Stemming makes doc 1 match first
    assert.equal(results[1].id, 2);
  }
  // Ranking functions
  {
    let results = Array.from(
      sql.exec(`
      SELECT *, bm25(documents_fts) FROM documents_fts WHERE documents_fts MATCH '2' ORDER BY rank;
    `)
    );
    assert.equal(results.length, 2);
    assert.equal(
      results[0]['bm25(documents_fts)'] < results[1]['bm25(documents_fts)'],
      true
    ); // Better matches have lower bm25 (since they're all negative
    assert.equal(results[0].id, 2); // Doc 2 comes first (sorted by rank)
    assert.equal(results[1].id, 1);
  }
  // highlight() function
  {
    let results = Array.from(
      sql.exec(`
        SELECT highlight(documents_fts, 2, '<b>', '</b>') as output FROM documents_fts WHERE documents_fts MATCH '2' ORDER BY rank;
    `)
    );
    assert.equal(results.length, 2);
    assert.equal(
      results[0].output,
      `This is the content of document <b>2</b> (of <b>2</b>).`
    ); // two matches, two highlights
    assert.equal(
      results[1].output,
      `This is the contents of document 1 (of <b>2</b>).`
    );
  }
  // snippet() function
  {
    let results = Array.from(
      sql.exec(`
        SELECT snippet(documents_fts, 2, '<b>', '</b>', '...', 4) as output FROM documents_fts WHERE documents_fts MATCH '2' ORDER BY rank;
    `)
    );
    assert.equal(results.length, 2);
    assert.equal(results[0].output, `...document <b>2</b> (of <b>2</b>).`); // two matches, two highlights
    assert.equal(results[1].output, `...document 1 (of <b>2</b>).`);
  }

  // Complex queries

  // List table info
  {
    let result = [
      ...sql.exec(`
        SELECT name as tbl_name,
               ncol as num_columns
        FROM pragma_table_list
        WHERE TYPE = "table"
          AND tbl_name NOT LIKE "sqlite_%"
          AND tbl_name NOT LIKE "d1_%"
          AND tbl_name NOT LIKE "_cf_%"`),
    ];
    assert.equal(result.length, 2);
    assert.equal(result[0].tbl_name, 'myTable');
    assert.equal(result[0].num_columns, 2);
    assert.equal(result[1].tbl_name, 'documents');
    assert.equal(result[1].num_columns, 3);
  }

  // Similar query using JSON objects
  {
    const jsonResult = JSON.parse(
      Array.from(
        sql.exec(
          `SELECT json_group_array(json_object(
              'type', type,
              'name', name,
              'tbl_name', tbl_name,
              'rootpage', rootpage,
              'sql', sql,
              'columns', (SELECT json_group_object(name, type) from pragma_table_info(tbl_name))
          )) as data
       FROM sqlite_master
       WHERE type = "table" AND tbl_name != "_cf_KV";`
        )
      )[0].data
    );
    assert.equal(jsonResult.length, 11);
    assert.equal(
      jsonResult.map((r) => r.name).join(','),
      'myTable,documents,documents_fts,documents_fts_data,documents_fts_idx,documents_fts_content,documents_fts_docsize,documents_fts_config,documents_fts_v_col,documents_fts_v_row,documents_fts_v_instance'
    );
    assert.equal(jsonResult[0].columns.foo, 'TEXT');
    assert.equal(jsonResult[0].columns.bar, 'INTEGER');
    assert.equal(jsonResult[1].columns.id, 'INTEGER');
    assert.equal(jsonResult[1].columns.title, 'TEXT');
    assert.equal(jsonResult[1].columns.content, 'TEXT');
  }

  let assertValidBool = (name, val) => {
    sql.exec('PRAGMA defer_foreign_keys = ' + name + ';');
    assert.equal(
      [...sql.exec('PRAGMA defer_foreign_keys;')][0].defer_foreign_keys,
      val
    );
  };
  let assertInvalidBool = (name, msg) => {
    assert.throws(
      () => sql.exec('PRAGMA defer_foreign_keys = ' + name + ';'),
      msg || /not authorized/
    );
  };

  assertValidBool('true', 1);
  assertValidBool('false', 0);
  assertValidBool('on', 1);
  assertValidBool('off', 0);
  assertValidBool('yes', 1);
  assertValidBool('no', 0);
  assertValidBool('1', 1);
  assertValidBool('0', 0);

  // case-insensitive
  assertValidBool('tRuE', 1);
  assertValidBool('NO', 0);

  // quoted
  assertValidBool("'true'", 1);
  assertValidBool('"yes"', 1);
  assertValidBool('"0"', 0);

  // whitespace is trimmed by sqlite before passing to authorizer
  assertValidBool('  true    ', 1);

  // Don't accept anything invalid...
  assertInvalidBool('abcd');
  assertInvalidBool('"foo"');
  assertInvalidBool("'yes", 'unrecognized token');

  // Test database size interface.
  assert.equal(sql.databaseSize, 36864);
  sql.exec(`CREATE TABLE should_make_one_more_page(VALUE text);`);
  assert.equal(sql.databaseSize, 36864 + 4096);
  sql.exec(`DROP TABLE should_make_one_more_page;`);
  assert.equal(sql.databaseSize, 36864);

  storage.put('txnTest', 0);

  // Try a transaction while no implicit transaction is open.
  await scheduler.wait(1); // finish implicit txn
  let txnResult = await storage.transaction(async () => {
    storage.put('txnTest', 1);
    assert.equal(await storage.get('txnTest'), 1);
    return 'foo';
  });
  assert.equal(await storage.get('txnTest'), 1);
  assert.equal(txnResult, 'foo');

  // Try a transaction while an implicit transaction is open first.
  storage.put('txnTest', 2);
  await storage.transaction(async () => {
    storage.put('txnTest', 3);
    assert.equal(await storage.get('txnTest'), 3);
  });
  assert.equal(await storage.get('txnTest'), 3);

  // Try a transaction that is explicitly rolled back.
  await storage.transaction(async (txn) => {
    storage.put('txnTest', 4);
    assert.equal(await storage.get('txnTest'), 4);
    txn.rollback();
  });
  assert.equal(await storage.get('txnTest'), 3);

  // Try a transaction that is implicitly rolled back by throwing an exception.
  try {
    await storage.transaction(async (txn) => {
      storage.put('txnTest', 5);
      assert.equal(await storage.get('txnTest'), 5);
      throw new Error('txn failure');
    });
    throw new Error('expected errror');
  } catch (err) {
    assert.equal(err.message, 'txn failure');
  }
  assert.equal(await storage.get('txnTest'), 3);

  // Try a nested transaction.
  await storage.transaction(async (txn) => {
    storage.put('txnTest', 6);
    assert.equal(await storage.get('txnTest'), 6);
    await storage.transaction(async (txn2) => {
      storage.put('txnTest', 7);
      assert.equal(await storage.get('txnTest'), 7);
      // Let's even do an await in here for good measure.
      await scheduler.wait(1);
    });
    assert.equal(await storage.get('txnTest'), 7);
    txn.rollback();
  });
  assert.equal(await storage.get('txnTest'), 3);

  // Test transactionSync, success
  {
    await scheduler.wait(1);
    const result = storage.transactionSync(() => {
      sql.exec('CREATE TABLE IF NOT EXISTS should_succeed (VALUE text);');
      return 'some data';
    });

    assert.equal(result, 'some data');

    const results = Array.from(
      sql.exec(`
      SELECT * FROM sqlite_master WHERE tbl_name = 'should_succeed'
    `)
    );
    assert.equal(results.length, 1);
  }

  // Test transactionSync, failure
  {
    await scheduler.wait(1);

    assert.throws(
      () =>
        storage.transactionSync(() => {
          sql.exec('CREATE TABLE should_be_rolled_back (VALUE text);');
          sql.exec('SELECT * FROM misspelled_table_name;');
        }),
      'Error: no such table: misspelled_table_name'
    );

    const results = Array.from(
      sql.exec(`
      SELECT * FROM sqlite_master WHERE tbl_name = 'should_be_rolled_back'
    `)
    );
    assert.equal(results.length, 0);
  }

  // Test transactionSync, nested
  {
    sql.exec('CREATE TABLE txnTest (i INTEGER)');
    sql.exec('INSERT INTO txnTest VALUES (1)');

    let setI = sql.prepare('UPDATE txnTest SET i = ?');
    let getIStmt = sql.prepare('SELECT i FROM txnTest');
    let getI = () => [...getIStmt()][0].i;

    assert.equal(getI(), 1);
    storage.transactionSync(() => {
      setI(2);
      assert.equal(getI(), 2);

      assert.throws(
        () =>
          storage.transactionSync(() => {
            setI(3);
            assert.equal(getI(), 3);
            throw new Error('foo');
          }),
        'Error: foo'
      );

      assert.equal(getI(), 2);
    });
    assert.equal(getI(), 2);
  }

  // Test joining two tables with overlapping names
  {
    sql.exec(`CREATE TABLE abc (a INT, b INT, c INT);`);
    sql.exec(`CREATE TABLE cde (c INT, d INT, e INT);`);
    sql.exec(`INSERT INTO abc VALUES (1,2,3),(4,5,6);`);
    sql.exec(`INSERT INTO cde VALUES (7,8,9),(1,2,3);`);

    const stmt = sql.prepare(`SELECT * FROM abc, cde`);

    // In normal iteration, data is lost
    const objResults = Array.from(stmt());
    assert.equal(Object.values(objResults[0]).length, 5); // duplicate column 'c' dropped
    assert.equal(Object.values(objResults[1]).length, 5); // duplicate column 'c' dropped
    assert.equal(Object.values(objResults[2]).length, 5); // duplicate column 'c' dropped
    assert.equal(Object.values(objResults[3]).length, 5); // duplicate column 'c' dropped

    assert.equal(objResults[0].c, 7); // Value of 'c' is the second in the join
    assert.equal(objResults[1].c, 1); // Value of 'c' is the second in the join
    assert.equal(objResults[2].c, 7); // Value of 'c' is the second in the join
    assert.equal(objResults[3].c, 1); // Value of 'c' is the second in the join

    // Iterator has a 'columnNames' property, with .raw() that lets us get the full data
    const iterator = stmt();
    assert.deepEqual(iterator.columnNames, ['a', 'b', 'c', 'c', 'd', 'e']);
    const rawResults = Array.from(iterator.raw());
    assert.equal(rawResults.length, 4);
    assert.deepEqual(rawResults[0], [1, 2, 3, 7, 8, 9]);
    assert.deepEqual(rawResults[1], [1, 2, 3, 1, 2, 3]);
    assert.deepEqual(rawResults[2], [4, 5, 6, 7, 8, 9]);
    assert.deepEqual(rawResults[3], [4, 5, 6, 1, 2, 3]);

    // Once an iterator is consumed, it can no longer access the columnNames.
    assert.throws(() => {
      iterator.columnNames;
    }, 'Error: Cannot call .getColumnNames after Cursor iterator has been consumed.');

    // Also works with cursors returned from .exec
    const execIterator = sql.exec(`SELECT * FROM abc, cde`);
    assert.deepEqual(execIterator.columnNames, ['a', 'b', 'c', 'c', 'd', 'e']);
    assert.equal(Array.from(execIterator.raw())[0].length, 6);
  }

  await scheduler.wait(1);

  // Test for bug where a cursor constructed from a prepared statement didn't have a strong ref
  // to the statement object.
  {
    sql.exec('CREATE TABLE iteratorTest (i INTEGER)');
    sql.exec('INSERT INTO iteratorTest VALUES (0), (1)');

    let q = sql.prepare('SELECT * FROM iteratorTest')();
    let i = 0;
    for (let row of q) {
      assert.equal(row.i, i++);
      gc();
    }
  }

  {
    // Test binding blobs & nulls
    sql.exec(`CREATE TABLE test_blob (id INTEGER PRIMARY KEY, data BLOB);`);
    sql.prepare(
      `INSERT INTO test_blob(data) VALUES(?),(ZEROBLOB(10)),(null),(?);`
    )(crypto.getRandomValues(new Uint8Array(12)), null);
    const results = Array.from(sql.exec(`SELECT * FROM test_blob`));
    assert.equal(results.length, 4);
    assert.equal(results[0].data instanceof ArrayBuffer, true);
    assert.equal(results[0].data.byteLength, 12);
    assert.equal(results[1].data instanceof ArrayBuffer, true);
    assert.equal(results[1].data.byteLength, 10);
    assert.equal(results[2].data, null);
    assert.equal(results[3].data, null);
  }

  // Can rename tables
  sql.exec(`
    CREATE TABLE beforerename (
      id INTEGER
    );
  `);
  sql.exec(`
    ALTER TABLE beforerename
    RENAME TO afterrename;
  `);

  sql.exec(`
    CREATE TABLE altercolumns (
      meta TEXT
     );
  `);
  // Can add columns
  sql.exec(`
    ALTER TABLE altercolumns
    ADD COLUMN tobedeleted TEXT;
  `);
  // Can rename columns within a table
  sql.exec(`
    ALTER TABLE altercolumns
    RENAME COLUMN meta TO metadata
  `);
  // Can drop columns
  sql.exec(`
    ALTER TABLE altercolumns
    DROP COLUMN tobedeleted
  `);

  // Can add columns with a CHECK
  sql.exec(`
    ALTER TABLE altercolumns
    ADD COLUMN checked_col TEXT CHECK(checked_col IN ('A','B'));
  `);

  // The CHECK is enforced unless `ignore_check_constraints` is on
  sql.exec(`INSERT INTO altercolumns(checked_col) VALUES ('A')`);
  assert.throws(
    () => sql.exec(`INSERT INTO altercolumns(checked_col) VALUES ('C')`),
    /Error: CHECK constraint failed: checked_col IN \('A','B'\)/
  );

  // Because there's already a row, adding another column with a CHECK
  // but no default value will fail
  assert.throws(
    () =>
      sql.exec(`
        ALTER TABLE altercolumns
        ADD COLUMN second_col TEXT CHECK(second_col IS NOT NULL);
      `),
    /Error: CHECK constraint failed/
  );

  // ignore_check_constraints lets us bypass this for adding bad data
  sql.exec(`PRAGMA ignore_check_constraints=ON;`);
  sql.exec(`INSERT INTO altercolumns(checked_col) VALUES ('C')`);
  assert.deepEqual(
    [...sql.exec(`SELECT * FROM altercolumns`)],
    [
      { checked_col: 'A', metadata: null },
      { checked_col: 'C', metadata: null },
    ]
  );

  // Or even adding columns that start broken (because second_col is NULL)
  sql.exec(`
    ALTER TABLE altercolumns
    ADD COLUMN second_col TEXT CHECK(second_col IS NOT NULL);
  `);

  // Turning check constraints back on doesn't actually do any checking, eagerly
  sql.exec(`PRAGMA ignore_check_constraints=OFF;`);

  // But anything else that CHECKs that table will now fail, like adding another CHECK
  assert.throws(
    () =>
      sql.exec(`
        ALTER TABLE altercolumns
        ADD COLUMN third_col TEXT DEFAULT 'E' CHECK(third_col IN ('E','F'));
      `),
    /Error: CHECK constraint failed/
  );

  // And we can use quick_check to list out that there are now errors
  // (although these messages aren't great):
  assert.deepEqual(
    [...sql.exec(`PRAGMA quick_check;`)],
    [
      { quick_check: 'CHECK constraint failed in altercolumns' },
      { quick_check: 'CHECK constraint failed in altercolumns' },
    ]
  );

  // Can't create another temp table
  assert.throws(
    () =>
      sql.exec(`
    CREATE TEMP TABLE tempy AS
      SELECT * FROM sqlite_master;
  `),
    'Error: not authorized'
  );

  // Assert foreign keys can be truly turned off, not just deferred
  await state.blockConcurrencyWhile(async () => {
    sql.exec(`PRAGMA foreign_keys = OFF;`);
  });
  storage.transactionSync(() => {
    sql.exec(`
      CREATE TABLE A (
        id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
        bId INTEGER NOT NULL REFERENCES B (id) ON DELETE RESTRICT ON UPDATE CASCADE
      );
      INSERT INTO A VALUES(1,1); -- this would throw a parse error with foreign keys on
      CREATE TABLE B (
        id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT
      );
    `);
  });

  // Until we've inserted the row into B, we can detect our
  // foreign key violation (even with foreign_keys=OFF)
  assert.deepEqual(Array.from(sql.exec(`pragma foreign_key_check;`)), [
    { table: 'A', rowid: 1, parent: 'B', fkid: 0 },
  ]);
  sql.exec(`INSERT INTO B VALUES (1);`);
  assert.deepEqual(Array.from(sql.exec(`pragma foreign_key_check;`)), []);

  // Restore foreign keys for the rest of the tests
  await state.blockConcurrencyWhile(async () => {
    sql.exec(`PRAGMA foreign_keys = ON;`);
  });
}

async function testIoStats(storage) {
  const sql = storage.sql;

  sql.exec(`CREATE TABLE tbl (id INTEGER PRIMARY KEY, value TEXT)`);
  sql.exec(
    `INSERT INTO tbl (id, value) VALUES (?, ?)`,
    100000,
    'arbitrary-initial-value'
  );
  await scheduler.wait(1);

  // When writing, the rowsWritten count goes up.
  {
    const cursor = sql.exec(
      `INSERT INTO tbl (id, value) VALUES (?, ?)`,
      1,
      'arbitrary-value'
    );
    Array.from(cursor); // Consume all the results
    assert.equal(cursor.rowsWritten, 1);
  }

  // When reading, the rowsRead count goes up.
  {
    const cursor = sql.exec(`SELECT * FROM tbl`);
    Array.from(cursor); // Consume all the results
    assert.equal(cursor.rowsRead, 2);
  }

  // Each invocation of a prepared statement gets its own counters.
  {
    const id1 = 101;
    const id2 = 202;

    const prepared = sql.prepare(`INSERT INTO tbl (id, value) VALUES (?, ?)`);
    const cursor123 = prepared(id1, 'value1');
    Array.from(cursor123);
    assert.equal(cursor123.rowsWritten, 1);

    const cursor456 = prepared(id2, 'value2');
    Array.from(cursor456);
    assert.equal(cursor456.rowsWritten, 1);
    assert.equal(cursor123.rowsWritten, 1); // remained unchanged
  }

  // Row counters are updated as you consume the cursor.
  {
    sql.exec(`DELETE FROM tbl`);
    const prepared = sql.prepare(`INSERT INTO tbl (id, value) VALUES (?, ?)`);
    for (let i = 1; i <= 10; i++) {
      Array.from(prepared(i, 'value' + i));
    }

    const cursor = sql.exec(`SELECT * FROM tbl`);
    const resultsIterator = cursor[Symbol.iterator]();
    let rowsSeen = 0;
    while (true) {
      const result = resultsIterator.next();
      if (result.done) {
        break;
      }
      assert.equal(++rowsSeen, cursor.rowsRead);
    }
  }

  // Row counters can track interleaved cursors
  {
    const join = [];
    const colCounts = [];
    // In-JS joining of two tables should be possible:
    const rows = sql.exec(`SELECT * FROM abc`);
    for (let row of rows) {
      const cols = sql.exec(`SELECT * FROM cde`);
      for (let col of cols) {
        join.push({ row, col });
      }
      colCounts.push(cols.rowsRead);
    }
    assert.deepEqual(join, [
      { col: { c: 7, d: 8, e: 9 }, row: { a: 1, b: 2, c: 3 } },
      { col: { c: 1, d: 2, e: 3 }, row: { a: 1, b: 2, c: 3 } },
      { col: { c: 7, d: 8, e: 9 }, row: { a: 4, b: 5, c: 6 } },
      { col: { c: 1, d: 2, e: 3 }, row: { a: 4, b: 5, c: 6 } },
    ]);
    assert.deepEqual(rows.rowsRead, 2);
    assert.deepEqual(colCounts, [2, 2]);
  }

  // Temporary tables (i.e. for IN clauses) don't contribute to rowsWritten
  {
    const cursor = sql.exec(`SELECT * FROM abc WHERE a IN (1,2,3,4,5,6)`);
    const rows = Array.from(cursor);
    assert.deepEqual(cursor.rowsRead, 2);
    assert.deepEqual(cursor.rowsWritten, 0);
  }
}

async function testForeignKeys(storage) {
  const sql = storage.sql;

  // Test defer_foreign_keys
  {
    sql.exec(`CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);`);
    sql.exec(
      `CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER, content TEXT, FOREIGN KEY(user_id) REFERENCES users(id));`
    );

    await scheduler.wait(1);

    // By default, primary keys are enforced:
    assert.throws(
      () =>
        sql.exec(
          `INSERT INTO posts (user_id, content) VALUES (?, ?)`,
          1,
          'Post 1'
        ),
      /Error: FOREIGN KEY constraint failed/
    );

    // Transactions fail immediately too
    let passed_first_statement = false;
    assert.throws(
      () =>
        storage.transactionSync(() => {
          sql.exec(
            `INSERT INTO posts (user_id, content) VALUES (?, ?)`,
            1,
            'Post 1'
          );
          passed_first_statement = true;
        }),
      /Error: FOREIGN KEY constraint failed/
    );
    assert.equal(passed_first_statement, false);

    await scheduler.wait(1);

    // With defer_foreign_keys, we can insert things out-of-order within transactions,
    // as long as the data is valid by the end.
    storage.transactionSync(() => {
      sql.exec(`PRAGMA defer_foreign_keys=ON;`);
      sql.exec(
        `INSERT INTO posts (user_id, content) VALUES (?, ?)`,
        1,
        'Post 1'
      );
      sql.exec(`INSERT INTO users VALUES (?, ?)`, 1, 'Alice');
    });

    await scheduler.wait(1);

    // But if we use defer_foreign_keys but try to commit, it resets the DO
    storage.transactionSync(() => {
      sql.exec(`PRAGMA defer_foreign_keys=ON;`);
      sql.exec(
        `INSERT INTO posts (user_id, content) VALUES (?, ?)`,
        2,
        'Post 2'
      );
    });
  }
}

async function testStreamingIngestion(request, storage) {
  const { sql } = storage;

  sql.exec(`CREATE TABLE streaming(val TEXT);`);

  await storage.transaction(async () => {
    const stream = request.body.pipeThrough(new TextDecoderStream());
    let buffer = '';

    for await (const chunk of stream) {
      // Append the new chunk to the existing buffer
      buffer += chunk;

      // Ingest any complete statements and snip those chars off the buffer
      buffer = sql.ingest(buffer).remainder;
    }
  });

  // Verify exactly 36 rows were added
  assert.deepEqual(Array.from(sql.exec(`SELECT count(*) FROM streaming`)), [
    { 'count(*)': 36 },
  ]);
  assert.deepEqual(
    Array.from(sql.exec(`SELECT * FROM streaming WHERE val LIKE 'f%'`)),
    [
      { val: 'f: 😳' },
      { val: 'f: 🫠' },
      { val: 'f: 🙃' },
      { val: 'f: 🤡' },
      { val: 'f: 🥺' },
      { val: 'f: 🔥😎🔥' },
    ]
  );
}

export class DurableObjectExample extends DurableObject {
  constructor(state, env) {
    super(state, env);
    this.state = state;
  }

  async fetch(req) {
    if (req.url.endsWith('/sql-test')) {
      await test(this.state);
      return Response.json({ ok: true });
    } else if (req.url.endsWith('/sql-test-foreign-keys')) {
      await testForeignKeys(this.state.storage);
      return Response.json({ ok: true });
    } else if (req.url.endsWith('/increment')) {
      let val = (await this.state.storage.get('counter')) || 0;
      ++val;
      this.state.storage.put('counter', val);
      return Response.json(val);
    } else if (req.url.endsWith('/break')) {
      // This `put()` should be discarded due to the actor aborting immediately after.
      this.state.storage.put('counter', 888);

      // Abort the actor, which also cancels unflushed writes.
      this.state.abort('test broken');

      // abort() always throws.
      throw new Error("can't get here");
    } else if (req.url.endsWith('/sql-test-io-stats')) {
      await testIoStats(this.state.storage);
      return Response.json({ ok: true });
    } else if (req.url.endsWith('/streaming-ingestion')) {
      await testStreamingIngestion(req, this.state.storage);
      return Response.json({ ok: true });
    } else if (req.url.endsWith('/deleteAll')) {
      this.state.storage.put('counter', 888); // will be deleted
      this.state.storage.deleteAll();
      assert.strictEqual(await this.state.storage.get('counter'), undefined);
      return Response.json({ ok: true });
    }

    throw new Error('unknown url: ' + req.url);
  }

  async testRollbackKvInit() {
    // Test what happens if initialization of the _cf_KV table gets rolled back.

    try {
      this.state.storage.transactionSync(() => {
        // Cause KV table to be initialized.
        this.state.storage.put('foo', 123);

        // Roll back the transaction by throwing.
        throw new Error('bar');
      });
      throw new Error('expected error');
    } catch (err) {
      if (err.message != 'bar') throw err;
    }

    // Now try to put to KV again. This will create the `_cf_KV` table again.
    await this.state.storage.put('foo', 456);
  }

  async testRollbackAlarmInit() {
    // Much like testRollbackKvInit() but for alarms.

    try {
      this.state.storage.transactionSync(() => {
        // Cause KV table to be initialized.
        this.state.storage.setAlarm(Date.now() + 86400 * 365);

        // Roll back the transaction by throwing.
        throw new Error('bar');
      });
      throw new Error('expected error');
    } catch (err) {
      if (err.message != 'bar') throw err;
    }

    assert.strictEqual(await this.state.storage.getAlarm(), null);
    await this.state.storage.setAlarm(Date.now() + 86400 * 365);
  }

  async alarm() {}

  async testMultiStatement() {
    // Performing this PRAGMA will cause sqlite to invalidate prepared statements and re-compile
    // them the next time they are executed. (Probably, many other pragmas would have the same
    // effect, but this is the one that we observed causing issues.)
    //
    // In particular, the prepared statement ActorSqlite::beginTxn, which is simply
    // `BEGIN TRANSACTION`, will be invalidated and recomplied on the next invocation.
    //
    // When we perform our multi-statement exec below, the first line will invoke the
    // `ActorSqlite::onWrite` callback, which will invoke `beginTxn`. Because `BEGIN TRANSACTION`
    // must be recompiled, the SQLite authorizer callback will be invoked to check if it is
    // authorized. But we use the authorizer callback to detect when SQLite has parsed a statement
    // as a transaction statement. At one point, we had a bug where we incorrectly thought that
    // the authorizer was being called on behalf of the statement we were trying to parse and
    // execute, namely, `CREATE TABLE items...`. We therefore incorrectly made note that this
    // statement was beginning a transaction. This led the transaction state tracking to become
    // all wrong!
    //
    // This only turned out to be an issue when performing a multi-statement exec(), because in
    // this case all statements except the last are executed inside the parse loop, which is why
    // we misinterpreted the authorizer callback.
    this.state.storage.sql.exec('PRAGMA case_sensitive_like = TRUE');

    let cursor = this.state.storage.sql.exec(`
      CREATE TABLE items(i INTEGER, s TEXT);
      CREATE INDEX itemsIdx ON items(s);
      INSERT INTO items VALUES (123, "abc");
      INSERT INTO items VALUES (456, "def");
      SELECT i FROM items WHERE s = "abc";
    `);

    assert.deepEqual([...cursor], [{ i: 123 }]);
  }
}

export default {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName('A');
    let obj = env.ns.get(id);

    // Now let's test persistence through breakage and atomic write coalescing.
    let doReq = async (path, init = {}) => {
      let resp = await obj.fetch('http://foo/' + path, init);
      return await resp.json();
    };

    // Test SQL API
    assert.deepEqual(await doReq('sql-test'), { ok: true });

    // Test SQL IO stats
    assert.deepEqual(await doReq('sql-test-io-stats'), { ok: true });

    // Test SQL streaming ingestion
    assert.deepEqual(
      await doReq('streaming-ingestion', {
        method: 'POST',
        body: new ReadableStream({
          async start(controller) {
            const data = new TextEncoder().encode(INSERT_36_ROWS);

            // Pick a value for chunkSize that splits the first emoji in half
            const chunkSize = INSERT_36_ROWS.indexOf('😳') + 1;
            assert.equal(chunkSize, 35); // Validate we're getting the value we expect

            // Send each chunk with a wait of 1ms in between
            for (
              let offset = 0;
              offset < data.length - 1;
              offset += chunkSize
            ) {
              controller.enqueue(data.slice(offset, offset + chunkSize));
              await scheduler.wait(1);
            }

            controller.close();
          },
        }),
      }),
      { ok: true }
    );

    // Test defer_foreign_keys (explodes the DO)
    await assert.rejects(async () => {
      await doReq('sql-test-foreign-keys');
    }, /constraints were violated: FOREIGN KEY constraint failed: SQLITE_CONSTRAINT/);

    // Some increments.
    assert.equal(await doReq('increment'), 1);
    assert.equal(await doReq('increment'), 2);

    // Now induce a failure.
    await assert.rejects(
      async () => {
        await doReq('break');
      },
      (err) => err.message === 'test broken' && err.durableObjectReset
    );

    // Get a new stub.
    obj = env.ns.get(id);

    // Everything's still consistent.
    assert.equal(await doReq('increment'), 3);

    // Delete all: increments start over
    await doReq('deleteAll');
    assert.equal(await doReq('increment'), 1);
    assert.equal(await doReq('increment'), 2);
  },
};

export let testRollbackKvInit = {
  async test(ctrl, env, ctx) {
    let stub = env.ns.get(env.ns.idFromName('rollback-kv-test'));
    await stub.testRollbackKvInit();
    await stub.testRollbackAlarmInit();
  },
};

export let testMultiStatement = {
  async test(ctrl, env, ctx) {
    let stub = env.ns.get(env.ns.idFromName('multi-statement-test'));
    await stub.testMultiStatement();
  },
};

const INSERT_36_ROWS = ['a', 'b', 'c', 'd', 'e', 'f']
  .map(
    (prefix) =>
      `INSERT INTO streaming VALUES ${['😳', '🫠', '🙃', '🤡', '🥺', '🔥😎🔥']
        .map((suffix) => `('${prefix}: ${suffix}')`)
        .join(',')};`
  )
  .join(' ');
