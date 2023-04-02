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
  const resultBinding = [...sql.exec("SELECT ?", {bindValues: [456]})];
  assert.equal(resultBinding.length, 1);
  assert.equal(resultBinding[0]["?"], 456);

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

  // Prepared statement with binding values
  const preparedWithBinding = sql.prepare("SELECT ?");
  const resultPreparedWithBinding = [...preparedWithBinding({bindValues: [789]})];
  assert.equal(resultPreparedWithBinding.length, 1);
  assert.equal(resultPreparedWithBinding[0]["?"], 789);

  // Prepared statement (incorrect number of binding values)
  requireException(() => preparedWithBinding({bindValues: []}),
    "Error: Wrong number of parameter bindings for SQL query.");

  // Create and access hidden _cf_ table as admin
  sql.exec("CREATE TABLE _cf_test (name TEXT)", {admin: true});
  sql.exec("SELECT * FROM _cf_test", {admin: true});

  // Accessing a hidden _cf_ table
  requireException(() => sql.exec("CREATE TABLE _cf_invalid (name TEXT)"),
    "not authorized");
  requireException(() => sql.exec("SELECT * FROM _cf_test"),
    "access to _cf_test.name is prohibited");

  // Accessing pragmas without and withn admin
  requireException(() => sql.exec("PRAGMA hard_heap_limit = 1024"),
    "not authorized");
  sql.exec("PRAGMA hard_heap_limit = 1024", {admin: true});

  // Transactions without and withn admin
  requireException(() => sql.exec("BEGIN TRANSACTION; END TRANSACTION"),
    "not authorized");
  sql.exec("BEGIN TRANSACTION; END TRANSACTION", {admin: true});
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
