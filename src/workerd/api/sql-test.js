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

function test(sql) {
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
  // Test binding values
  const resultBinding = [...sql.exec("SELECT ?", 456)];
  assert.equal(resultBinding.length, 1);
  assert.equal(resultBinding[0]["?"], 456);

  // Test multiple binding values
  const resultBinding2 = [...sql.exec("SELECT ? + ?", 123, 456)];
  assert.equal(resultBinding2.length, 1);
  assert.equal(resultBinding2[0]["? + ?"], 579);

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

  // Can't get table_info for _cf_KV.
  requireException(() => sql.exec("PRAGMA table_info(_cf_KV)"), "not authorized");

  // Basic functions like abs() work.
  assert.equal([...sql.exec("SELECT abs(-123)").raw()][0][0], 123);

  // We don't permit sqlite_*() functions.
  requireException(() => sql.exec("SELECT sqlite_version()"),
      "not authorized to use function: sqlite_version");

  // At present we don't permit JSON. This includes the -> operator, which is considered a
  // function.
  requireException(() => sql.exec("SELECT '{\"a\":2,\"c\":[4,5,{\"f\":7}]}' -> '$'"),
      "not authorized to use function: ->");
}

export class DurableObjectExample {
  constructor(state, env) {
    this.state = state;
  }

  async fetch() {
    test(this.state.storage.sql);
    return new Response();
  }
}

export default {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName("A");
    let obj = env.ns.get(id);
    await obj.fetch("http://foo");
  }
}
