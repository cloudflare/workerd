// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

export class UdfTestDO extends DurableObject {
  constructor(state, env) {
    super(state, env);
    this.state = state;
  }

  // ===========================================================================
  // Basic UDF Functionality Tests
  // ===========================================================================

  async testSanityCheck() {
    const sql = this.state.storage.sql;
    const result = sql.exec('SELECT 42 AS answer').one();
    assert.strictEqual(result.answer, 42);
  }

  async testCreateFunctionExists() {
    const sql = this.state.storage.sql;
    assert.strictEqual(typeof sql.createFunction, 'function');
  }

  async testCreateFunctionValidatesName() {
    const sql = this.state.storage.sql;

    assert.throws(() => sql.createFunction('', () => 42), {
      name: 'TypeError',
      message: 'Function name cannot be empty.',
    });

    const longName = 'x'.repeat(256);
    assert.throws(() => sql.createFunction(longName, () => 42), {
      name: 'TypeError',
      message: 'Function name is too long (max 255 bytes).',
    });

    const maxName = 'x'.repeat(255);
    sql.createFunction(maxName, () => 42);
  }

  async testSimpleScalarUdf() {
    const sql = this.state.storage.sql;
    sql.createFunction('my_double', (x) => x * 2);
    const result = sql.exec('SELECT my_double(21) AS answer').one();
    assert.strictEqual(result.answer, 42);
  }

  async testUdfWithMultipleArgs() {
    const sql = this.state.storage.sql;
    sql.createFunction('my_add', (a, b) => a + b);
    const result = sql.exec('SELECT my_add(10, 32) AS answer').one();
    assert.strictEqual(result.answer, 42);
  }

  async testUdfWithStringResult() {
    const sql = this.state.storage.sql;
    sql.createFunction('greet', (name) => `Hello, ${name}!`);
    const result = sql.exec("SELECT greet('World') AS greeting").one();
    assert.strictEqual(result.greeting, 'Hello, World!');
  }

  async testUdfReturnsNull() {
    const sql = this.state.storage.sql;
    sql.createFunction('get_null', () => null);
    const result = sql.exec('SELECT get_null() AS val').one();
    assert.strictEqual(result.val, null);
  }

  // ===========================================================================
  // Error Handling Tests
  //
  // These tests verify that JavaScript exceptions thrown within UDFs are
  // properly caught and propagated to the caller.
  // ===========================================================================

  async testUdfThrowsError() {
    const sql = this.state.storage.sql;
    sql.createFunction('throw_error', () => {
      throw new Error('UDF error message');
    });
    assert.throws(
      () => sql.exec('SELECT throw_error() AS val').one(),
      (err) => err.message.includes('UDF error message')
    );
  }

  async testUdfThrowsTypeError() {
    const sql = this.state.storage.sql;
    sql.createFunction('throw_type_error', () => {
      throw new TypeError('Invalid type in UDF');
    });
    assert.throws(
      () => sql.exec('SELECT throw_type_error() AS val').one(),
      (err) => err.message.includes('Invalid type in UDF')
    );
  }

  async testUdfThrowsString() {
    const sql = this.state.storage.sql;
    sql.createFunction('throw_string', () => {
      throw 'string error message';
    });
    assert.throws(
      () => sql.exec('SELECT throw_string() AS val').one(),
      (err) => err instanceof Error
    );
  }

  async testUdfThrowsNumber() {
    const sql = this.state.storage.sql;
    sql.createFunction('throw_number', () => {
      throw 42;
    });
    assert.throws(
      () => sql.exec('SELECT throw_number() AS val').one(),
      (err) => err instanceof Error
    );
  }

  async testUdfThrowsNull() {
    const sql = this.state.storage.sql;
    sql.createFunction('throw_null', () => {
      throw null;
    });
    assert.throws(
      () => sql.exec('SELECT throw_null() AS val').one(),
      (err) => err instanceof Error
    );
  }

  async testUdfThrowsUndefined() {
    const sql = this.state.storage.sql;
    sql.createFunction('throw_undefined', () => {
      throw undefined;
    });
    assert.throws(
      () => sql.exec('SELECT throw_undefined() AS val').one(),
      (err) => err instanceof Error
    );
  }

  async testUdfThrowsObject() {
    const sql = this.state.storage.sql;
    sql.createFunction('throw_object', () => {
      throw { custom: 'error object', code: 123 };
    });
    assert.throws(
      () => sql.exec('SELECT throw_object() AS val').one(),
      (err) => err instanceof Error
    );
  }

  async testUdfErrorStopsIteration() {
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE err_iter_test (id INTEGER)');
    sql.exec('INSERT INTO err_iter_test VALUES (1), (2), (3), (4), (5)');

    let callCount = 0;
    sql.createFunction('fail_on_third', (id) => {
      callCount++;
      if (id === 3) {
        throw new Error('Failed on id 3');
      }
      return id * 10;
    });

    let error = null;
    const results = [];
    try {
      for (const row of sql.exec(
        'SELECT fail_on_third(id) as val FROM err_iter_test ORDER BY id'
      )) {
        results.push(row.val);
      }
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE err_iter_test');

    // Should have called UDF 3 times (1, 2, 3) before error
    assert.strictEqual(callCount, 3, 'UDF should have been called 3 times');
    // Should have collected results for ids 1 and 2 before the error
    assert.ok(
      results.length <= 2,
      'Should have at most 2 results before error'
    );
    // Should have received an error
    assert.ok(error !== null, 'Should have received an error');
    assert.ok(
      error.message.includes('Failed on id 3'),
      'Error message should be preserved'
    );
  }

  async testUdfErrorInWhereClause() {
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE err_where_test (id INTEGER)');
    sql.exec('INSERT INTO err_where_test VALUES (1), (2), (3)');

    sql.createFunction('fail_in_where', (id) => {
      if (id === 2) {
        throw new Error('WHERE clause error');
      }
      return id > 0 ? 1 : 0;
    });

    let error = null;
    try {
      sql
        .exec('SELECT * FROM err_where_test WHERE fail_in_where(id) = 1')
        .toArray();
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE err_where_test');

    assert.ok(error !== null, 'Should have received an error');
    assert.ok(
      error.message.includes('WHERE clause error'),
      'Error message should be preserved'
    );
  }

  async testUdfErrorWithErrorCause() {
    const sql = this.state.storage.sql;
    sql.createFunction('throw_with_cause', () => {
      const cause = new Error('Root cause');
      throw new Error('Wrapper error', { cause });
    });
    assert.throws(
      () => sql.exec('SELECT throw_with_cause() AS val').one(),
      (err) => err.message.includes('Wrapper error')
    );
  }

  async testUdfReturnsUndefined() {
    const sql = this.state.storage.sql;
    sql.createFunction('get_undefined', () => {});
    const result = sql.exec('SELECT get_undefined() AS val').one();
    assert.strictEqual(result.val, null);
  }

  async testUdfWithBlobInput() {
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE IF NOT EXISTS blob_test (data BLOB)');
    sql.exec(
      'INSERT INTO blob_test VALUES (?)',
      new Uint8Array([1, 2, 3, 4, 5])
    );

    sql.createFunction('blob_len', (blob) => {
      if (blob instanceof Uint8Array) return blob.length;
      if (blob instanceof ArrayBuffer) return blob.byteLength;
      if (ArrayBuffer.isView(blob)) return blob.byteLength;
      return -1;
    });

    const result = sql
      .exec('SELECT blob_len(data) AS len FROM blob_test')
      .one();
    assert.strictEqual(result.len, 5);

    sql.exec('DROP TABLE blob_test');
  }

  async testUdfWithBlobOutput() {
    const sql = this.state.storage.sql;

    sql.createFunction('make_blob', (size) => {
      const arr = new Uint8Array(size);
      for (let i = 0; i < size; i++) arr[i] = i % 256;
      return arr;
    });

    const result = sql.exec('SELECT make_blob(5) AS blob').one();
    assert.ok(
      result.blob instanceof Uint8Array || result.blob instanceof ArrayBuffer
    );
    const view = new Uint8Array(result.blob);
    assert.strictEqual(view.length, 5);
    assert.strictEqual(view[0], 0);
    assert.strictEqual(view[4], 4);
  }

  // ===========================================================================
  // Re-entrancy Tests
  //
  // These tests verify that attempting to execute SQL from within a UDF
  // callback is properly blocked.
  //
  // SQL Entry Points (ways to start a query):
  //   - sql.exec()
  //   - sql.prepare().run()
  //   - sql.ingest()
  //
  // Cursor Entry Points (ways to advance/consume a query):
  //   - cursor.next()
  //   - cursor.toArray()
  //   - cursor.one()
  //   - cursor[Symbol.iterator] (for...of)
  //   - cursor.raw()[Symbol.iterator]
  //
  // SQL Contexts where UDFs are invoked:
  //   - SELECT column
  //   - WHERE clause
  //   - INSERT VALUES
  //   - UPDATE SET
  // ===========================================================================

  // Helper to create a UDF that attempts re-entrant SQL and tracks execution
  _createReentrantUdf(sql, name, reentrantCall) {
    let callCount = 0;
    let successCount = 0;

    sql.createFunction(name, (...args) => {
      callCount++;
      reentrantCall(sql, args);
      successCount++;
      return 42;
    });

    return {
      getCallCount: () => callCount,
      getSuccessCount: () => successCount,
    };
  }

  // Helper to assert re-entrancy was blocked
  _assertReentrancyBlocked(tracker, description) {
    assert.ok(
      tracker.getCallCount() > 0,
      `${description}: UDF should have been called`
    );
    assert.strictEqual(
      tracker.getSuccessCount(),
      0,
      `${description}: UDF should NOT have completed - re-entrant SQL should have been blocked`
    );
  }

  // ---------------------------------------------------------------------------
  // Re-entrancy via SQL Entry Points
  // ---------------------------------------------------------------------------

  async testReentrancyViaExec() {
    const sql = this.state.storage.sql;
    const tracker = this._createReentrantUdf(sql, 'reenter_exec', (sql) => {
      sql.exec('SELECT 1').one();
    });

    assert.throws(
      () => sql.exec('SELECT reenter_exec() AS val').one(),
      (err) => err instanceof Error
    );
    this._assertReentrancyBlocked(tracker, 'sql.exec()');
  }

  async testReentrancyViaPreparedStatement() {
    const sql = this.state.storage.sql;
    const tracker = this._createReentrantUdf(sql, 'reenter_prepare', (sql) => {
      sql.prepare('SELECT 1 AS val').run().one();
    });

    assert.throws(
      () => sql.exec('SELECT reenter_prepare() AS val').one(),
      (err) => err instanceof Error
    );
    this._assertReentrancyBlocked(tracker, 'sql.prepare().run()');
  }

  async testReentrancyViaIngest() {
    const sql = this.state.storage.sql;
    const tracker = this._createReentrantUdf(sql, 'reenter_ingest', (sql) => {
      sql.ingest('SELECT 1;');
    });

    assert.throws(
      () => sql.exec('SELECT reenter_ingest() AS val').one(),
      (err) => err instanceof Error
    );
    this._assertReentrancyBlocked(tracker, 'sql.ingest()');
  }

  // ---------------------------------------------------------------------------
  // Re-entrancy via Cursor Entry Points
  // ---------------------------------------------------------------------------

  async testReentrancyViaCursorNext() {
    const sql = this.state.storage.sql;
    const tracker = this._createReentrantUdf(sql, 'reenter_next', (sql) => {
      sql.exec('SELECT 1 AS val').next();
    });

    assert.throws(
      () => sql.exec('SELECT reenter_next() AS val').one(),
      (err) => err instanceof Error
    );
    this._assertReentrancyBlocked(tracker, 'cursor.next()');
  }

  async testReentrancyViaCursorToArray() {
    const sql = this.state.storage.sql;
    const tracker = this._createReentrantUdf(sql, 'reenter_toarray', (sql) => {
      sql.exec('SELECT 1 AS val').toArray();
    });

    assert.throws(
      () => sql.exec('SELECT reenter_toarray() AS val').one(),
      (err) => err instanceof Error
    );
    this._assertReentrancyBlocked(tracker, 'cursor.toArray()');
  }

  async testReentrancyViaCursorOne() {
    const sql = this.state.storage.sql;
    const tracker = this._createReentrantUdf(sql, 'reenter_one', (sql) => {
      sql.exec('SELECT 1 AS val').one();
    });

    assert.throws(
      () => sql.exec('SELECT reenter_one() AS val').one(),
      (err) => err instanceof Error
    );
    this._assertReentrancyBlocked(tracker, 'cursor.one()');
  }

  async testReentrancyViaCursorIterator() {
    const sql = this.state.storage.sql;
    const tracker = this._createReentrantUdf(sql, 'reenter_iterator', (sql) => {
      for (const row of sql.exec('SELECT 1 AS val')) {
        // iterate
      }
    });

    assert.throws(
      () => sql.exec('SELECT reenter_iterator() AS val').one(),
      (err) => err instanceof Error
    );
    this._assertReentrancyBlocked(tracker, 'cursor[Symbol.iterator]');
  }

  async testReentrancyViaCursorRaw() {
    const sql = this.state.storage.sql;
    const tracker = this._createReentrantUdf(sql, 'reenter_raw', (sql) => {
      for (const row of sql.exec('SELECT 1 AS val').raw()) {
        // iterate raw
      }
    });

    assert.throws(
      () => sql.exec('SELECT reenter_raw() AS val').one(),
      (err) => err instanceof Error
    );
    this._assertReentrancyBlocked(tracker, 'cursor.raw()');
  }

  // ---------------------------------------------------------------------------
  // Re-entrancy in Various SQL Contexts
  // ---------------------------------------------------------------------------

  async testReentrancyDuringCursorIteration() {
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE iter_test (id INTEGER)');
    sql.exec('INSERT INTO iter_test VALUES (1), (2), (3)');

    const tracker = this._createReentrantUdf(sql, 'reenter_iter', (sql) => {
      sql.exec('SELECT COUNT(*) FROM iter_test').one();
    });

    let error = null;
    try {
      for (const row of sql.exec('SELECT reenter_iter(id) FROM iter_test')) {
        // Should not complete
      }
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE iter_test');

    assert.ok(error !== null, 'Should have thrown during iteration');
    this._assertReentrancyBlocked(tracker, 'multi-row iteration');
  }

  async testReentrancyInWhereClause() {
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE where_test (id INTEGER)');
    sql.exec('INSERT INTO where_test VALUES (1), (2), (3)');

    const tracker = this._createReentrantUdf(sql, 'reenter_where', (sql) => {
      sql.exec('SELECT 1').one();
    });

    let error = null;
    try {
      sql
        .exec('SELECT * FROM where_test WHERE reenter_where(id) > 0')
        .toArray();
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE where_test');

    assert.ok(error !== null, 'Should have thrown during WHERE evaluation');
    this._assertReentrancyBlocked(tracker, 'WHERE clause');
  }

  async testReentrancyInInsert() {
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE insert_test (id INTEGER, computed INTEGER)');

    const tracker = this._createReentrantUdf(sql, 'reenter_insert', (sql) => {
      sql.exec('SELECT 1').one();
    });

    let error = null;
    try {
      sql.exec('INSERT INTO insert_test VALUES (1, reenter_insert(1))');
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE insert_test');

    assert.ok(error !== null, 'Should have thrown during INSERT');
    this._assertReentrancyBlocked(tracker, 'INSERT statement');
  }

  async testReentrancyInUpdate() {
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE update_test (id INTEGER, value INTEGER)');
    sql.exec('INSERT INTO update_test VALUES (1, 100)');

    const tracker = this._createReentrantUdf(sql, 'reenter_update', (sql) => {
      sql.exec('SELECT 1').one();
    });

    let error = null;
    try {
      sql.exec('UPDATE update_test SET value = reenter_update(id)');
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE update_test');

    assert.ok(error !== null, 'Should have thrown during UPDATE');
    this._assertReentrancyBlocked(tracker, 'UPDATE statement');
  }

  // ===========================================================================
  // Non-Re-entrancy Tests (should succeed)
  // ===========================================================================

  async testNestedUdfCallsAllowed() {
    // UDF calling another UDF is NOT re-entrancy - both execute within same sqlite3_step()
    const sql = this.state.storage.sql;

    sql.createFunction('inner_udf', (x) => x * 2);
    sql.createFunction('outer_udf', (x) => x + 10);

    const result = sql.exec('SELECT outer_udf(inner_udf(5)) AS val').one();
    assert.strictEqual(result.val, 20);
  }

  async testQueriesBetweenCursorIterationsAllowed() {
    // Running queries BETWEEN cursor iterations (not inside UDF) is allowed
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE between_test (id INTEGER)');
    sql.exec('INSERT INTO between_test VALUES (1), (2), (3)');

    const results = [];
    for (const row of sql.exec('SELECT id FROM between_test ORDER BY id')) {
      // This runs AFTER sqlite3_step() returns, so it's safe
      const count = sql.exec('SELECT COUNT(*) as cnt FROM between_test').one();
      results.push({ id: row.id, count: count.cnt });
    }

    sql.exec('DROP TABLE between_test');

    assert.strictEqual(results.length, 3);
    assert.strictEqual(results[0].id, 1);
    assert.strictEqual(results[0].count, 3);
  }

  async testSequentialQueriesAllowed() {
    // Multiple sequential queries with UDFs should work
    const sql = this.state.storage.sql;

    sql.createFunction('seq_udf', (x) => x * 2);

    const r1 = sql.exec('SELECT seq_udf(5) AS val').one();
    assert.strictEqual(r1.val, 10);

    const r2 = sql.exec('SELECT 42 AS val').one();
    assert.strictEqual(r2.val, 42);

    const r3 = sql.exec('SELECT seq_udf(10) AS val').one();
    assert.strictEqual(r3.val, 20);
  }
}

// =============================================================================
// Test Exports
// =============================================================================

export default {
  async test(ctrl, env, ctx) {
    const id = env.ns.idFromName('udf-test');
    const stub = env.ns.get(id);

    // Basic functionality tests
    await stub.testSanityCheck();
    await stub.testCreateFunctionExists();
    await stub.testCreateFunctionValidatesName();
    await stub.testSimpleScalarUdf();
    await stub.testUdfWithMultipleArgs();
    await stub.testUdfWithStringResult();
    await stub.testUdfReturnsNull();

    // Error handling tests
    await stub.testUdfThrowsError();
    await stub.testUdfThrowsTypeError();
    await stub.testUdfThrowsString();
    await stub.testUdfThrowsNumber();
    await stub.testUdfThrowsNull();
    await stub.testUdfThrowsUndefined();
    await stub.testUdfThrowsObject();
    await stub.testUdfErrorStopsIteration();
    await stub.testUdfErrorInWhereClause();
    await stub.testUdfErrorWithErrorCause();

    await stub.testUdfReturnsUndefined();
    await stub.testUdfWithBlobInput();
    await stub.testUdfWithBlobOutput();

    // Re-entrancy tests: SQL entry points (should be blocked)
    await stub.testReentrancyViaExec();
    await stub.testReentrancyViaPreparedStatement();
    await stub.testReentrancyViaIngest();

    // Re-entrancy tests: Cursor entry points (should be blocked)
    await stub.testReentrancyViaCursorNext();
    await stub.testReentrancyViaCursorToArray();
    await stub.testReentrancyViaCursorOne();
    await stub.testReentrancyViaCursorIterator();
    await stub.testReentrancyViaCursorRaw();

    // Re-entrancy tests: Various SQL contexts (should be blocked)
    await stub.testReentrancyDuringCursorIteration();
    await stub.testReentrancyInWhereClause();
    await stub.testReentrancyInInsert();
    await stub.testReentrancyInUpdate();

    // Non-re-entrancy tests (should succeed)
    await stub.testNestedUdfCallsAllowed();
    await stub.testQueriesBetweenCursorIterationsAllowed();
    await stub.testSequentialQueriesAllowed();
  },
};
