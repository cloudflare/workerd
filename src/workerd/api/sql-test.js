import * as assert from 'node:assert'

function requireException(callback, expectStr) {
  try {
    callback();
  } catch (err) {
    const errStr = `${err}`;
    if (!errStr.includes(expectStr)) {
      throw new Error(`Got unexpected exception '${errStr}', expected: '${expectStr}'`);
    }
    return;
  }
  throw new Error(`Expected exception '${expectStr}' but none was thrown`);
}

async function test(storage) {
  const sql = storage.sql
  // Test numeric results
  const resultNumber = [...sql.exec("SELECT 123")];
  assert.equal(resultNumber.length, 1);
  assert.equal(resultNumber[0]["123"], 123);

  // Test raw results
  const resultNumberRaw = [...sql.exec("SELECT 123").raw()];
  assert.equal(resultNumberRaw.length, 1);
  assert.equal(resultNumberRaw[0].length, 1);
  assert.equal(resultNumberRaw[0][0], 123);

  // Test string results
  const resultStr = [...sql.exec("SELECT 'hello'")];
  assert.equal(resultStr.length, 1);
  assert.equal(resultStr[0]["'hello'"], "hello");

  // Test blob results
  const resultBlob = [...sql.exec("SELECT x'ff' as blob")];
  assert.equal(resultBlob.length, 1);
  const blob = new Uint8Array(resultBlob[0].blob);
  assert.equal(blob.length, 1);
  assert.equal(blob[0], 255);

  {
    // Test binding values
    const result = [...sql.exec("SELECT ?", 456)];
    assert.equal(result.length, 1);
    assert.equal(result[0]["?"], 456);
  }

  {
    // Test multiple binding values
    const result = [...sql.exec("SELECT ? + ?", 123, 456)];
    assert.equal(result.length, 1);
    assert.equal(result[0]["? + ?"], 579);
  }

  {
    // Test multiple rows
    const result = [...sql.exec("SELECT 1 AS value\n" +
        "UNION ALL\n" +
        "SELECT 2 AS value\n" +
        "UNION ALL\n" +
        "SELECT 3 AS value;")];
    assert.equal(result.length, 3);
    assert.equal(result[0]["value"], 1);
    assert.equal(result[1]["value"], 2);
    assert.equal(result[2]["value"], 3);
  }

  // Test count
  {
    const result = [...sql.exec("SELECT count(value) from (SELECT 1 AS value\n" +
        "UNION ALL\n" +
        "SELECT 2 AS value\n" +
        "UNION ALL\n" +
        "SELECT 3 AS value);")];
    assert.equal(result.length, 1);
    assert.equal(result[0]["count(value)"], 3);
  }

  // Test sum
  {
    const result = [...sql.exec("SELECT sum(value) from (SELECT 1 AS value\n" +
        "UNION ALL\n" +
        "SELECT 2 AS value\n" +
        "UNION ALL\n" +
        "SELECT 3 AS value);")];
    assert.equal(result.length, 1);
    assert.equal(result[0]["sum(value)"], 6);
  }

  // Empty statements
  requireException(() => sql.exec(""),
    "SQL code did not contain a statement");
  requireException(() => sql.exec(";"),
    "SQL code did not contain a statement");

  // Invalid statements
  requireException(() => sql.exec("SELECT ;"),
    "syntax error");

  // Incorrect number of binding values
  requireException(() => sql.exec("SELECT ?"),
    "Error: Wrong number of parameter bindings for SQL query.");

  // Prepared statement
  const prepared = sql.prepare("SELECT 789");
  const resultPrepared = [...prepared()];
  assert.equal(resultPrepared.length, 1);
  assert.equal(resultPrepared[0]["789"], 789);

  // Running the same query twice invalidates the previous cursor.
  let result1 = prepared();
  let result2 = prepared();
  assert.equal([...result2][0]["789"], 789);
  requireException(() => [...result1],
      "SQL cursor was closed because the same statement was executed again.");

  // That said if a cursor was already done before the statement was re-run, it's not considered
  // canceled.
  prepared();
  assert.equal([...result2].length, 0);

  // Prepared statement with binding values
  const preparedWithBinding = sql.prepare("SELECT ?");
  const resultPreparedWithBinding = [...preparedWithBinding(789)];
  assert.equal(resultPreparedWithBinding.length, 1);
  assert.equal(resultPreparedWithBinding[0]["?"], 789);

  // Prepared statement (incorrect number of binding values)
  requireException(() => preparedWithBinding(),
    "Error: Wrong number of parameter bindings for SQL query.");

  // Accessing a hidden _cf_ table
  requireException(() => sql.exec("CREATE TABLE _cf_invalid (name TEXT)"),
    "not authorized");
  requireException(() => sql.exec("SELECT * FROM _cf_KV"),
    "access to _cf_KV.key is prohibited");

  // Some pragmas are completely not allowed
  requireException(() => sql.exec("PRAGMA hard_heap_limit = 1024"),
    "not authorized");

  // Test reading read-only pragmas
  {
    const result = [...sql.exec("pragma data_version;")];
    assert.equal(result.length, 1);
    assert.equal(result[0]["data_version"], 2);
  }

  // Trying to write to read-only pragmas is not allowed
  requireException(() => sql.exec("PRAGMA data_version = 5"),
    "not authorized");
  requireException(() => sql.exec("PRAGMA max_page_count = 65536"),
    "not authorized");
  requireException(() => sql.exec("PRAGMA page_size = 8192"),
    "not authorized");

  // PRAGMA table_info is allowed.
  sql.exec("CREATE TABLE myTable (foo TEXT, bar INTEGER)");
  {
    let info = [...sql.exec("PRAGMA table_info(myTable)")];
    assert.equal(info.length, 2);
    assert.equal(info[0].name, "foo");
    assert.equal(info[1].name, "bar");
  }

  // Can't get table_info for _cf_KV.
  requireException(() => sql.exec("PRAGMA table_info(_cf_KV)"), "not authorized");

  // Basic functions like abs() work.
  assert.equal([...sql.exec("SELECT abs(-123)").raw()][0][0], 123);

  // We don't permit sqlite_*() functions.
  requireException(() => sql.exec("SELECT sqlite_version()"),
      "not authorized to use function: sqlite_version");

  // JSON -> operator works
  const jsonResult =
      [...sql.exec("SELECT '{\"a\":2,\"c\":[4,5,{\"f\":7}]}' -> '$.c' AS value")][0].value;
  assert.equal(jsonResult, "[4,5,{\"f\":7}]");

  // current_{date,time,timestamp} functions work
  const resultDate = [...sql.exec("SELECT current_date")];
  assert.equal(resultDate.length, 1);
  // Should match results in the format "2023-06-01"
  assert.match(resultDate[0]["current_date"], /^\d{4}-\d{2}-\d{2}$/)

  const resultTime = [...sql.exec("SELECT current_time")];
  assert.equal(resultTime.length, 1)
  // Should match results in the format "15:30:03"
  assert.match(resultTime[0]["current_time"], /^\d{2}:\d{2}:\d{2}$/)

  const resultTimestamp = [...sql.exec("SELECT current_timestamp")];
  assert.equal(resultTimestamp.length, 1);
  // Should match results in the format "2023-06-01 15:30:03"
  assert.match(resultTimestamp[0]["current_timestamp"], /^\d{4}-\d{2}-\d{2}\s{1}\d{2}:\d{2}:\d{2}$/)

  // Can't start transactions or savepoints.
  requireException(() => sql.exec("BEGIN TRANSACTION"), "not authorized");
  requireException(() => sql.exec("SAVEPOINT foo"), "not authorized");

  // Full text search extension

  sql.exec(`
    CREATE TABLE documents (
      id INTEGER PRIMARY KEY,
      title TEXT NOT NULL,
      content TEXT NOT NULL
    );
  `);
  sql.exec(`
    CREATE VIRTUAL TABLE documents_fts USING fts5(id, title, content, tokenize = porter);
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
    let results = Array.from(sql.exec(`
      SELECT * FROM documents_fts WHERE documents_fts MATCH 'content' ORDER BY rank;
    `));
    assert.equal(results.length, 2)
    assert.equal(results[0].id, 1) // Stemming makes doc 1 match first
    assert.equal(results[1].id, 2)
  }
  // Ranking functions
  {
    let results = Array.from(sql.exec(`
      SELECT *, bm25(documents_fts) FROM documents_fts WHERE documents_fts MATCH '2' ORDER BY rank;
    `));
    assert.equal(results.length, 2)
    assert.equal(results[0]['bm25(documents_fts)'] < results[1]['bm25(documents_fts)'], true) // Better matches have lower bm25 (since they're all negative
    assert.equal(results[0].id, 2) // Doc 2 comes first (sorted by rank)
    assert.equal(results[1].id, 1)
  }
  // highlight() function
  {
    let results = Array.from(sql.exec(`
        SELECT highlight(documents_fts, 2, '<b>', '</b>') as output FROM documents_fts WHERE documents_fts MATCH '2' ORDER BY rank;
    `));
    assert.equal(results.length, 2)
    assert.equal(results[0].output, `This is the content of document <b>2</b> (of <b>2</b>).`) // two matches, two highlights
    assert.equal(results[1].output, `This is the contents of document 1 (of <b>2</b>).`)
  }
  // snippet() function
  {
    let results = Array.from(sql.exec(`
        SELECT snippet(documents_fts, 2, '<b>', '</b>', '...', 4) as output FROM documents_fts WHERE documents_fts MATCH '2' ORDER BY rank;
    `));
    assert.equal(results.length, 2)
    assert.equal(results[0].output, `...document <b>2</b> (of <b>2</b>).`) // two matches, two highlights
    assert.equal(results[1].output, `...document 1 (of <b>2</b>).`)
  }

  // Complex queries

  // List table info
  {
    let result = [...sql.exec(`
        SELECT name as tbl_name,
               ncol as num_columns
        FROM pragma_table_list
        WHERE TYPE = "table"
          AND tbl_name NOT LIKE "sqlite_%"
          AND tbl_name NOT LIKE "d1_%"
          AND tbl_name NOT LIKE "_cf_%"`)];
    assert.equal(result.length, 2);
    assert.equal(result[0].tbl_name, 'myTable');
    assert.equal(result[0].num_columns, 2);
    assert.equal(result[1].tbl_name, 'documents');
    assert.equal(result[1].num_columns, 3);
  }

  // Similar query using JSON objects
  {
    const jsonResult = JSON.parse(Array.from(sql.exec(
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
    ))[0].data)
    assert.equal(jsonResult.length, 8)
    assert.equal(jsonResult.map(r => r.name).join(','), 'myTable,documents,documents_fts,documents_fts_data,documents_fts_idx,documents_fts_content,documents_fts_docsize,documents_fts_config')
    assert.equal(jsonResult[0].columns.foo, 'TEXT')
    assert.equal(jsonResult[0].columns.bar, 'INTEGER')
    assert.equal(jsonResult[1].columns.id, 'INTEGER')
    assert.equal(jsonResult[1].columns.title, 'TEXT')
    assert.equal(jsonResult[1].columns.content, 'TEXT')
  }

  // Let the current open transaction commit. We have to do this before playing with the
  // foreign_keys pragma because it doesn't work while a transaction is open.
  await scheduler.wait(1);

  let assertValidBool = (name, val) => {
    sql.exec("PRAGMA foreign_keys = " + name + ";");
    assert.equal([...sql.exec("PRAGMA foreign_keys;")][0].foreign_keys, val);
  };
  let assertInvalidBool = (name, msg) => {
    requireException(() => sql.exec("PRAGMA foreign_keys = " + name + ";"),
        msg || "not authorized");
  };

  assertValidBool("true", 1);
  assertValidBool("false", 0);
  assertValidBool("on", 1);
  assertValidBool("off", 0);
  assertValidBool("yes", 1);
  assertValidBool("no", 0);
  assertValidBool("1", 1);
  assertValidBool("0", 0);

  // case-insensitive
  assertValidBool("tRuE", 1);
  assertValidBool("NO", 0);

  // quoted
  assertValidBool("'true'", 1);
  assertValidBool("\"yes\"", 1);
  assertValidBool("\"0\"", 0);

  // whitespace is trimmed by sqlite before passing to authorizer
  assertValidBool("  true    ", 1);

  // Don't accept anything invalid...
  assertInvalidBool("abcd");
  assertInvalidBool("\"foo\"");
  assertInvalidBool("'yes", "unrecognized token");

  // Test database size interface.
  assert.equal(sql.databaseSize, 36864);
  sql.exec(`CREATE TABLE should_make_one_more_page(VALUE text);`)
  assert.equal(sql.databaseSize, 36864 + 4096);
  sql.exec(`DROP TABLE should_make_one_more_page;`)
  assert.equal(sql.databaseSize, 36864);

  storage.put("txnTest", 0);

  // Try a transaction while no implicit transaction is open.
  await scheduler.wait(1);  // finish implicit txn
  let txnResult = await storage.transaction(async () => {
    storage.put("txnTest", 1);
    assert.equal(await storage.get("txnTest"), 1);
    return "foo";
  });
  assert.equal(await storage.get("txnTest"), 1);
  assert.equal(txnResult, "foo");

  // Try a transaction while an implicit transaction is open first.
  storage.put("txnTest", 2);
  await storage.transaction(async () => {
    storage.put("txnTest", 3);
    assert.equal(await storage.get("txnTest"), 3);
  });
  assert.equal(await storage.get("txnTest"), 3);


  // Try a transaction that is explicitly rolled back.
  await storage.transaction(async txn => {
    storage.put("txnTest", 4);
    assert.equal(await storage.get("txnTest"), 4);
    txn.rollback();
  });
  assert.equal(await storage.get("txnTest"), 3);

  // Try a transaction that is implicitly rolled back by throwing an exception.
  try {
    await storage.transaction(async txn => {
      storage.put("txnTest", 5);
      assert.equal(await storage.get("txnTest"), 5);
      throw new Error("txn failure");
    });
    throw new Error("expected errror");
  } catch (err) {
    assert.equal(err.message, "txn failure");
  }
  assert.equal(await storage.get("txnTest"), 3);

  // Try a nested transaction.
  await storage.transaction(async txn => {
    storage.put("txnTest", 6);
    assert.equal(await storage.get("txnTest"), 6);
    await storage.transaction(async txn2 => {
      storage.put("txnTest", 7);
      assert.equal(await storage.get("txnTest"), 7);
      // Let's even do an await in here for good measure.
      await scheduler.wait(1);
    });
    assert.equal(await storage.get("txnTest"), 7);
    txn.rollback();
  });
  assert.equal(await storage.get("txnTest"), 3);

  // Test transactionSync, success
  {
    await scheduler.wait(1);
    const result = storage.transactionSync(() => {
      sql.exec("CREATE TABLE IF NOT EXISTS should_succeed (VALUE text);");
      return "some data"
    })

    assert.equal(result, "some data")

    const results = Array.from(sql.exec(`
      SELECT * FROM sqlite_master WHERE tbl_name = 'should_succeed'
    `))
    assert.equal(results.length, 1)
  }

  // Test transactionSync, failure
  {
    await scheduler.wait(1);

    requireException(() => storage.transactionSync(() => {
      sql.exec("CREATE TABLE should_be_rolled_back (VALUE text);");
      sql.exec("SELECT * FROM misspelled_table_name;")
    }), "Error: no such table: misspelled_table_name")

    const results = Array.from(sql.exec(`
      SELECT * FROM sqlite_master WHERE tbl_name = 'should_be_rolled_back'
    `))
    assert.equal(results.length, 0)
  }

  // Test transactionSync, nested
  {
    sql.exec("CREATE TABLE txnTest (i INTEGER)");
    sql.exec("INSERT INTO txnTest VALUES (1)");

    let setI = sql.prepare("UPDATE txnTest SET i = ?");
    let getIStmt = sql.prepare("SELECT i FROM txnTest");
    let getI = () => [...getIStmt()][0].i;

    assert.equal(getI(), 1);
    storage.transactionSync(() => {
      setI(2);
      assert.equal(getI(), 2);

      requireException(() => storage.transactionSync(() => {
        setI(3);
        assert.equal(getI(), 3);
        throw new Error("foo");
      }), "Error: foo");

      assert.equal(getI(), 2);
    });
    assert.equal(getI(), 2);
  }

  await scheduler.wait(1);

  // Test for bug where a cursor constructed from a prepared statement didn't have a strong ref
  // to the statement object.
  {
    sql.exec("CREATE TABLE iteratorTest (i INTEGER)");
    sql.exec("INSERT INTO iteratorTest VALUES (0), (1)");

    let q = sql.prepare("SELECT * FROM iteratorTest")();
    let i = 0;
    for (let row of q) {
      assert.equal(row.i, i++);
      gc();
    }
  }

  {
    // Test binding blobs & nulls
    sql.exec(`CREATE TABLE test_blob (id INTEGER PRIMARY KEY, data BLOB);`)
    sql.prepare(
      `INSERT INTO test_blob(data) VALUES(?),(ZEROBLOB(10)),(null),(?);`
    )(
      crypto.getRandomValues(new Uint8Array(12)), null
    )
    const results = Array.from(sql.exec(`SELECT * FROM test_blob`))
    assert.equal(results.length, 4)
    assert.equal(results[0].data instanceof ArrayBuffer, true)
    assert.equal(results[0].data.byteLength, 12)
    assert.equal(results[1].data instanceof ArrayBuffer, true)
    assert.equal(results[1].data.byteLength, 10)
    assert.equal(results[2].data, null)
    assert.equal(results[3].data, null)
  }

  // Can rename tables
  sql.exec(`
    CREATE TABLE beforerename (
      id INTEGER
    );
  `)
  sql.exec(`
    ALTER TABLE beforerename
    RENAME TO afterrename;
  `)

  // Can rename columns within a table
  sql.exec(`
    CREATE TABLE renamecolumn (
      meta TEXT
     );
  `);
  sql.exec(`
    ALTER TABLE renamecolumn
    RENAME COLUMN meta TO metadata
  `);
}

export class DurableObjectExample {
  constructor(state, env) {
    this.state = state;
  }

  async fetch(req) {
    if (req.url.endsWith("/sql-test")) {
      await test(this.state.storage);
      return new Response();
    } else if (req.url.endsWith("/increment")) {
      let val = (await this.state.storage.get("counter")) || 0;
      ++val;
      this.state.storage.put("counter", val);
      return Response.json(val);
    } else if (req.url.endsWith("/break")) {
      // This `put()` should be discarded due to the actor aborting immediately after.
      this.state.storage.put("counter", 888);

      // Abort the actor, which also cancels unflushed writes.
      this.state.abort("test broken");

      // abort() always throws.
      throw new Error("can't get here")
    }

    throw new Error("unknown url: " + req.url);
  }
}

export default {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName("A");
    let obj = env.ns.get(id);
    await obj.fetch("http://foo/sql-test");

    // Now let's test persistence through breakage and atomic write coalescing.
    let doReq = async path => {
      let resp = await obj.fetch("http://foo/" + path);
      return await resp.json();
    };

    // Some increments.
    assert.equal(await doReq("increment"), 1);
    assert.equal(await doReq("increment"), 2);

    // Now induce a failure.
    try {
      await doReq("break");
      throw new Error("expected failure");
    } catch (err) {
      if (err.message != "test broken") {
        throw err;
      }
      assert.equal(err.durableObjectReset, true);
    }

    // Get a new stub.
    obj = env.ns.get(id);

    // Everything's still consistent.
    assert.equal(await doReq("increment"), 3);
  }
}
