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

function test(sql, isSmall) {
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
  const resultBlob = [...sql.exec("SELECT x'ff'")];
  assert.equal(resultBlob.length, 1);
  const blob = new Uint8Array(resultBlob[0]["x'ff'"]);
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

  // Accessing pragmas is not allowed
  requireException(() => sql.exec("PRAGMA hard_heap_limit = 1024"),
    "not authorized");

  // PRAGMA table_info is allowed.
  sql.exec("CREATE TABLE myTable (foo TEXT, bar INTEGER)");
  {
    let info = [...sql.exec("PRAGMA table_info(myTable)")];
    assert.equal(info.length, 2);
    assert.equal(info[0].name, "foo");
    assert.equal(info[1].name, "bar");
  }
  {
    let info = [...sql.exec("PRAGMA page_count")];
    assert.equal(info.length, 1);
    assert.equal(info[0]["page_count"], 3);
  }

  // Only cap the small DBs
  if (isSmall) {
    let max_pages = [...sql.exec("PRAGMA max_page_count = 4")];
    assert.equal(max_pages.length, 1);
    assert.equal(max_pages[0]["max_page_count"], 4);
  }

  // Create a new table (4th page)
  sql.exec("CREATE TABLE newTable (foo TEXT, bar INTEGER)");
  {
    let info = [...sql.exec("PRAGMA page_count")];
    assert.equal(info.length, 1);
    assert.equal(info[0]["page_count"], 4);
  }

  if (isSmall) {
    // Should fail trying to make a 5th page
    requireException(() => sql.exec("CREATE TABLE newTable2 (foo TEXT, bar INTEGER)"),
      "Error: database or disk is full");
  } else {
    // Should have its own limits, not inherit the `PRAGMA max_page_count` from the Small one
    let max_pages = [...sql.exec("PRAGMA max_page_count")];
    assert.equal(max_pages.length, 1);
    assert.equal(max_pages[0]["max_page_count"], 1073741823);

    sql.exec("CREATE TABLE newTable2 (foo TEXT, bar INTEGER)")

    let info = [...sql.exec("PRAGMA page_count")];
    assert.equal(info.length, 1);
    assert.equal(info[0]["page_count"], 5);
  }

  // Can't get table_info for _cf_KV.
  requireException(() => sql.exec("PRAGMA table_info(_cf_KV)"), "not authorized");

  // Basic functions like abs() work.
  assert.equal([...sql.exec("SELECT abs(-123)").raw()][0][0], 123);

  // We don't permit sqlite_*() functions.
  requireException(() => sql.exec("SELECT sqlite_version()"),
      "not authorized to use function: sqlite_version");

  // JSON -> operator works
  let jsonResult =
      [...sql.exec("SELECT '{\"a\":2,\"c\":[4,5,{\"f\":7}]}' -> '$.c' AS value")][0].value;
  assert.equal(jsonResult, "[4,5,{\"f\":7}]");
}

export class DurableObjectExample {
  constructor(state, env) {
    this.state = state;
  }

  async fetch(request) {
    const url = new URL(request.url)
    test(this.state.storage.sql, url.pathname.endsWith('/small'));
    return new Response();
  }
}

export default {
  async test(ctrl, env, ctx) {
    {
      let id = env.ns.idFromName("A");
      let obj = env.ns.get(id);
      await obj.fetch("http://foo/small");
    }
    {
      let id = env.ns.idFromName("B");
      let obj = env.ns.get(id);
      await obj.fetch("http://foo/big");
    }
  }
}
