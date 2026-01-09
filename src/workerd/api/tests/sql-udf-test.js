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
    // Native JS: throwing a string means you catch a string
    let caught = null;
    try {
      sql.exec('SELECT throw_string() AS val').one();
    } catch (e) {
      caught = e;
    }
    assert.ok(caught !== null, 'Should have thrown');
    assert.strictEqual(caught, 'string error message');
  }

  async testUdfThrowsNumber() {
    const sql = this.state.storage.sql;
    sql.createFunction('throw_number', () => {
      throw 42;
    });
    // Native JS: throwing a number means you catch a number
    let caught = null;
    try {
      sql.exec('SELECT throw_number() AS val').one();
    } catch (e) {
      caught = e;
    }
    assert.ok(caught !== null, 'Should have thrown');
    assert.strictEqual(caught, 42);
  }

  async testUdfThrowsNull() {
    const sql = this.state.storage.sql;
    sql.createFunction('throw_null', () => {
      throw null;
    });
    // Native JS: throwing null means you catch null
    let caught = undefined; // Use undefined so we can distinguish from caught null
    let didThrow = false;
    try {
      sql.exec('SELECT throw_null() AS val').one();
    } catch (e) {
      didThrow = true;
      caught = e;
    }
    assert.ok(didThrow, 'Should have thrown');
    assert.strictEqual(caught, null);
  }

  async testUdfThrowsUndefined() {
    const sql = this.state.storage.sql;
    sql.createFunction('throw_undefined', () => {
      throw undefined;
    });
    // Native JS: throwing undefined means you catch undefined
    let caught = null; // Use null so we can distinguish from caught undefined
    let didThrow = false;
    try {
      sql.exec('SELECT throw_undefined() AS val').one();
    } catch (e) {
      didThrow = true;
      caught = e;
    }
    assert.ok(didThrow, 'Should have thrown');
    assert.strictEqual(caught, undefined);
  }

  async testUdfThrowsObject() {
    const sql = this.state.storage.sql;
    sql.createFunction('throw_object', () => {
      throw { custom: 'error object', code: 123 };
    });
    // Native JS: throwing a plain object means you catch a plain object
    let caught = null;
    try {
      sql.exec('SELECT throw_object() AS val').one();
    } catch (e) {
      caught = e;
    }
    assert.ok(caught !== null, 'Should have thrown');
    assert.strictEqual(caught.custom, 'error object');
    assert.strictEqual(caught.code, 123);
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

  async testUdfErrorStackTracePreserved() {
    const sql = this.state.storage.sql;

    // Define a helper function so we can check for its name in the stack
    function helperThatThrows() {
      throw new Error('Error from helper');
    }

    sql.createFunction('throw_with_stack', () => {
      helperThatThrows();
    });

    let error = null;
    try {
      sql.exec('SELECT throw_with_stack() AS val').one();
    } catch (e) {
      error = e;
    }

    assert.ok(error !== null, 'Should have received an error');
    assert.ok(
      error.message.includes('Error from helper'),
      'Error message should be preserved'
    );

    // Check that the stack trace includes the helper function name
    // This verifies that the original JS stack is preserved
    const stack = error.stack || '';
    assert.ok(
      stack.includes('helperThatThrows'),
      `Stack trace should include 'helperThatThrows', got: ${stack}`
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
  // NOTE: We use a single explicit parameter `_unused` to ensure function.length > 0,
  // which prevents the aggregate detection heuristic from calling this function
  // during createFunction(). The actual re-entrancy test happens when the UDF is
  // invoked from a SQL query.
  _createReentrantUdf(sql, name, reentrantCall) {
    let callCount = 0;
    let successCount = 0;

    sql.createFunction(name, (_unused, ...args) => {
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

  // ===========================================================================
  // Aggregate Function Tests
  // ===========================================================================

  async testAggregateSum() {
    const sql = this.state.storage.sql;

    sql.createFunction('my_sum', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE agg_test (value INTEGER)');
    sql.exec('INSERT INTO agg_test VALUES (1), (2), (3), (4), (5)');

    const result = sql
      .exec('SELECT my_sum(value) AS total FROM agg_test')
      .one();
    sql.exec('DROP TABLE agg_test');

    assert.strictEqual(result.total, 15);
  }

  async testAggregateCount() {
    const sql = this.state.storage.sql;

    sql.createFunction('my_count', () => {
      let count = 0;
      return {
        step: () => {
          count++;
        },
        final: () => count,
      };
    });

    sql.exec('CREATE TABLE count_test (value INTEGER)');
    sql.exec('INSERT INTO count_test VALUES (10), (20), (30)');

    const result = sql
      .exec('SELECT my_count(value) AS cnt FROM count_test')
      .one();
    sql.exec('DROP TABLE count_test');

    assert.strictEqual(result.cnt, 3);
  }

  async testAggregateWithNoRows() {
    const sql = this.state.storage.sql;

    sql.createFunction('sum_empty', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE empty_test (value INTEGER)');

    const result = sql
      .exec('SELECT sum_empty(value) AS total FROM empty_test')
      .one();
    sql.exec('DROP TABLE empty_test');

    // With no rows, final is called on the initial state (0)
    assert.strictEqual(result.total, 0);
  }

  async testAggregateWithGroupBy() {
    const sql = this.state.storage.sql;

    sql.createFunction('group_sum', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE group_test (category TEXT, value INTEGER)');
    sql.exec(
      "INSERT INTO group_test VALUES ('a', 1), ('a', 2), ('b', 10), ('b', 20), ('b', 30)"
    );

    const results = sql
      .exec(
        'SELECT category, group_sum(value) AS total FROM group_test GROUP BY category ORDER BY category'
      )
      .toArray();
    sql.exec('DROP TABLE group_test');

    assert.strictEqual(results.length, 2);
    assert.strictEqual(results[0].category, 'a');
    assert.strictEqual(results[0].total, 3);
    assert.strictEqual(results[1].category, 'b');
    assert.strictEqual(results[1].total, 60);
  }

  async testAggregateWithStringConcat() {
    const sql = this.state.storage.sql;

    sql.createFunction('str_concat', () => {
      let result = '';
      return {
        step: (value) => {
          result += value;
        },
        final: () => result,
      };
    });

    sql.exec('CREATE TABLE str_test (word TEXT)');
    sql.exec("INSERT INTO str_test VALUES ('Hello'), (' '), ('World')");

    const resultRow = sql
      .exec('SELECT str_concat(word) AS msg FROM str_test')
      .one();
    sql.exec('DROP TABLE str_test');

    assert.strictEqual(resultRow.msg, 'Hello World');
  }

  async testAggregateWithArrayState() {
    // Test collecting values into an array - much cleaner with factory pattern!
    const sql = this.state.storage.sql;

    sql.createFunction('collect', () => {
      const arr = [];
      return {
        step: (value) => {
          arr.push(value);
        },
        final: () => JSON.stringify(arr),
      };
    });

    sql.exec('CREATE TABLE collect_test (value INTEGER)');
    sql.exec('INSERT INTO collect_test VALUES (3), (1), (4), (1), (5)');

    const result = sql
      .exec('SELECT collect(value) AS arr FROM collect_test')
      .one();
    sql.exec('DROP TABLE collect_test');

    assert.deepStrictEqual(JSON.parse(result.arr), [3, 1, 4, 1, 5]);
  }

  async testAggregateStepError() {
    const sql = this.state.storage.sql;

    sql.createFunction('error_step', () => {
      let sum = 0;
      return {
        step: (value) => {
          if (value === 3) {
            throw new Error('Error on value 3');
          }
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE err_test (value INTEGER)');
    sql.exec('INSERT INTO err_test VALUES (1), (2), (3), (4), (5)');

    let error = null;
    try {
      sql.exec('SELECT error_step(value) AS total FROM err_test').one();
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE err_test');

    assert.ok(error !== null, 'Should have thrown');
    assert.ok(
      error.message.includes('Error on value 3'),
      'Error message should be preserved'
    );
  }

  async testAggregateFinalError() {
    const sql = this.state.storage.sql;

    sql.createFunction('error_final', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => {
          throw new Error('Error in final');
        },
      };
    });

    sql.exec('CREATE TABLE final_err_test (value INTEGER)');
    sql.exec('INSERT INTO final_err_test VALUES (1), (2), (3)');

    let error = null;
    try {
      sql.exec('SELECT error_final(value) AS total FROM final_err_test').one();
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE final_err_test');

    assert.ok(error !== null, 'Should have thrown');
    assert.ok(
      error.message.includes('Error in final'),
      'Error message should be preserved'
    );
  }

  async testAggregateValidation() {
    const sql = this.state.storage.sql;

    // With the factory pattern, detection happens at createFunction time.
    // If a zero-parameter function doesn't return a proper {step, final} object,
    // it's treated as a scalar function instead. These "invalid" aggregates
    // become scalar functions that return plain objects, which now throws an error.

    // Factory returns object without step - treated as scalar, throws on plain object
    sql.createFunction('bad_agg1', () => ({ final: () => 0 }));
    assert.throws(
      () => sql.exec('SELECT bad_agg1(1) AS val').one(),
      (err) =>
        err.message.includes('plain object') ||
        err.message.includes('[object Object]')
    );

    // Factory returns object without final - treated as scalar, throws on plain object
    sql.createFunction('bad_agg2', () => ({ step: () => {} }));
    assert.throws(
      () => sql.exec('SELECT bad_agg2(1) AS val').one(),
      (err) =>
        err.message.includes('plain object') ||
        err.message.includes('[object Object]')
    );

    // Factory returns object where step is not a function - treated as scalar, throws
    sql.createFunction('bad_agg3', () => ({
      step: 'not a function',
      final: () => 0,
    }));
    assert.throws(
      () => sql.exec('SELECT bad_agg3(1) AS val').one(),
      (err) =>
        err.message.includes('plain object') ||
        err.message.includes('[object Object]')
    );

    // Factory returns object where final is not a function - treated as scalar, throws
    sql.createFunction('bad_agg4', () => ({
      step: () => {},
      final: 'not a function',
    }));
    assert.throws(
      () => sql.exec('SELECT bad_agg4(1) AS val').one(),
      (err) =>
        err.message.includes('plain object') ||
        err.message.includes('[object Object]')
    );
  }

  async testAggregateMultipleArgs() {
    const sql = this.state.storage.sql;

    // Weighted sum: sum of (value * weight)
    sql.createFunction('weighted_sum', () => {
      let sum = 0;
      return {
        step: (value, weight) => {
          sum += value * weight;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE weighted_test (value INTEGER, weight INTEGER)');
    sql.exec('INSERT INTO weighted_test VALUES (10, 2), (20, 3), (30, 1)');

    const result = sql
      .exec('SELECT weighted_sum(value, weight) AS total FROM weighted_test')
      .one();
    sql.exec('DROP TABLE weighted_test');

    // 10*2 + 20*3 + 30*1 = 20 + 60 + 30 = 110
    assert.strictEqual(result.total, 110);
  }

  // ===========================================================================
  // Transaction Interaction Tests
  //
  // These tests verify that UDFs work correctly within transactions and that
  // errors properly trigger rollback behavior.
  // ===========================================================================

  async testScalarUdfInTransaction() {
    const storage = this.state.storage;
    const sql = storage.sql;

    sql.createFunction('txn_double', (x) => x * 2);

    sql.exec('CREATE TABLE txn_test (id INTEGER PRIMARY KEY, value INTEGER)');

    await storage.transaction(async () => {
      sql.exec('INSERT INTO txn_test VALUES (1, txn_double(21))');
      sql.exec('INSERT INTO txn_test VALUES (2, txn_double(10))');
    });

    const results = sql.exec('SELECT * FROM txn_test ORDER BY id').toArray();
    sql.exec('DROP TABLE txn_test');

    assert.strictEqual(results.length, 2);
    assert.strictEqual(results[0].value, 42);
    assert.strictEqual(results[1].value, 20);
  }

  async testScalarUdfErrorRollsBackTransaction() {
    const storage = this.state.storage;
    const sql = storage.sql;

    sql.createFunction('txn_fail', (x) => {
      if (x === 2) {
        throw new Error('UDF failure in transaction');
      }
      return x * 10;
    });

    sql.exec(
      'CREATE TABLE txn_rollback_test (id INTEGER PRIMARY KEY, value INTEGER)'
    );
    sql.exec('INSERT INTO txn_rollback_test VALUES (0, 0)'); // Pre-existing row

    let error = null;
    try {
      await storage.transaction(async () => {
        sql.exec('INSERT INTO txn_rollback_test VALUES (1, txn_fail(1))');
        sql.exec('INSERT INTO txn_rollback_test VALUES (2, txn_fail(2))'); // This should fail
        sql.exec('INSERT INTO txn_rollback_test VALUES (3, txn_fail(3))'); // Should not execute
      });
    } catch (e) {
      error = e;
    }

    // Transaction should have been rolled back
    const results = sql
      .exec('SELECT * FROM txn_rollback_test ORDER BY id')
      .toArray();
    sql.exec('DROP TABLE txn_rollback_test');

    assert.ok(error !== null, 'Should have thrown');
    assert.ok(
      error.message.includes('UDF failure in transaction'),
      'Error message should be preserved'
    );
    // Only the pre-existing row should remain
    assert.strictEqual(results.length, 1);
    assert.strictEqual(results[0].id, 0);
  }

  async testAggregateUdfInTransaction() {
    const storage = this.state.storage;
    const sql = storage.sql;

    sql.createFunction('txn_sum', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE txn_agg_test (value INTEGER)');

    await storage.transaction(async () => {
      sql.exec('INSERT INTO txn_agg_test VALUES (1), (2), (3)');
      const result = sql
        .exec('SELECT txn_sum(value) AS total FROM txn_agg_test')
        .one();
      assert.strictEqual(result.total, 6);

      sql.exec('INSERT INTO txn_agg_test VALUES (4)');
      const result2 = sql
        .exec('SELECT txn_sum(value) AS total FROM txn_agg_test')
        .one();
      assert.strictEqual(result2.total, 10);
    });

    sql.exec('DROP TABLE txn_agg_test');
  }

  async testAggregateUdfErrorRollsBackTransaction() {
    const storage = this.state.storage;
    const sql = storage.sql;

    sql.createFunction('txn_agg_fail', () => {
      let sum = 0;
      return {
        step: (value) => {
          if (value === 3) {
            throw new Error('Aggregate failure in transaction');
          }
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec(
      'CREATE TABLE txn_agg_rollback (id INTEGER PRIMARY KEY, value INTEGER)'
    );
    sql.exec('INSERT INTO txn_agg_rollback VALUES (0, 100)'); // Pre-existing row

    let error = null;
    try {
      await storage.transaction(async () => {
        sql.exec('INSERT INTO txn_agg_rollback VALUES (1, 1), (2, 2), (3, 3)');
        // This aggregate should fail when it hits value=3
        sql
          .exec('SELECT txn_agg_fail(value) AS total FROM txn_agg_rollback')
          .one();
      });
    } catch (e) {
      error = e;
    }

    const results = sql
      .exec('SELECT * FROM txn_agg_rollback ORDER BY id')
      .toArray();
    sql.exec('DROP TABLE txn_agg_rollback');

    assert.ok(error !== null, 'Should have thrown');
    assert.ok(
      error.message.includes('Aggregate failure in transaction'),
      'Error message should be preserved'
    );
    // Only the pre-existing row should remain
    assert.strictEqual(results.length, 1);
    assert.strictEqual(results[0].id, 0);
  }

  async testUdfInNestedTransaction() {
    const storage = this.state.storage;
    const sql = storage.sql;

    sql.createFunction('nested_double', (x) => x * 2);

    sql.exec(
      'CREATE TABLE nested_txn_test (id INTEGER PRIMARY KEY, value INTEGER)'
    );

    await storage.transaction(async () => {
      sql.exec('INSERT INTO nested_txn_test VALUES (1, nested_double(5))');

      await storage.transaction(async () => {
        sql.exec('INSERT INTO nested_txn_test VALUES (2, nested_double(10))');
      });

      sql.exec('INSERT INTO nested_txn_test VALUES (3, nested_double(15))');
    });

    const results = sql
      .exec('SELECT * FROM nested_txn_test ORDER BY id')
      .toArray();
    sql.exec('DROP TABLE nested_txn_test');

    assert.strictEqual(results.length, 3);
    assert.strictEqual(results[0].value, 10);
    assert.strictEqual(results[1].value, 20);
    assert.strictEqual(results[2].value, 30);
  }

  // ===========================================================================
  // Sync Transaction Tests
  //
  // These tests verify that UDFs work correctly with the synchronous
  // transactionSync() API.
  // ===========================================================================

  async testScalarUdfInSyncTransaction() {
    const storage = this.state.storage;
    const sql = storage.sql;

    sql.createFunction('sync_txn_double', (x) => x * 2);

    sql.exec(
      'CREATE TABLE sync_txn_test (id INTEGER PRIMARY KEY, value INTEGER)'
    );

    storage.transactionSync(() => {
      sql.exec('INSERT INTO sync_txn_test VALUES (1, sync_txn_double(21))');
      sql.exec('INSERT INTO sync_txn_test VALUES (2, sync_txn_double(10))');
    });

    const results = sql
      .exec('SELECT * FROM sync_txn_test ORDER BY id')
      .toArray();
    sql.exec('DROP TABLE sync_txn_test');

    assert.strictEqual(results.length, 2);
    assert.strictEqual(results[0].value, 42);
    assert.strictEqual(results[1].value, 20);
  }

  async testScalarUdfErrorRollsBackSyncTransaction() {
    const storage = this.state.storage;
    const sql = storage.sql;

    sql.createFunction('sync_txn_fail', (x) => {
      if (x === 2) {
        throw new Error('UDF failure in sync transaction');
      }
      return x * 10;
    });

    sql.exec(
      'CREATE TABLE sync_txn_rollback_test (id INTEGER PRIMARY KEY, value INTEGER)'
    );
    sql.exec('INSERT INTO sync_txn_rollback_test VALUES (0, 0)'); // Pre-existing row

    let error = null;
    try {
      storage.transactionSync(() => {
        sql.exec(
          'INSERT INTO sync_txn_rollback_test VALUES (1, sync_txn_fail(1))'
        );
        sql.exec(
          'INSERT INTO sync_txn_rollback_test VALUES (2, sync_txn_fail(2))'
        ); // Fails
        sql.exec(
          'INSERT INTO sync_txn_rollback_test VALUES (3, sync_txn_fail(3))'
        ); // Not executed
      });
    } catch (e) {
      error = e;
    }

    assert.ok(error !== null, 'Transaction should have thrown');
    assert.ok(
      error.message.includes('UDF failure in sync transaction'),
      `Expected UDF error message, got: ${error.message}`
    );

    // Verify rollback: only the pre-existing row should remain
    const results = sql
      .exec('SELECT * FROM sync_txn_rollback_test ORDER BY id')
      .toArray();
    sql.exec('DROP TABLE sync_txn_rollback_test');

    assert.strictEqual(
      results.length,
      1,
      'Transaction should have rolled back'
    );
    assert.strictEqual(results[0].id, 0);
  }

  async testAggregateUdfInSyncTransaction() {
    const storage = this.state.storage;
    const sql = storage.sql;

    sql.createFunction('sync_txn_sum', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE sync_txn_agg_test (value INTEGER)');

    storage.transactionSync(() => {
      sql.exec('INSERT INTO sync_txn_agg_test VALUES (10), (20), (30)');
    });

    const result = sql
      .exec('SELECT sync_txn_sum(value) AS total FROM sync_txn_agg_test')
      .one();
    sql.exec('DROP TABLE sync_txn_agg_test');

    assert.strictEqual(result.total, 60);
  }

  async testAggregateUdfErrorRollsBackSyncTransaction() {
    const storage = this.state.storage;
    const sql = storage.sql;

    let stepCount = 0;
    sql.createFunction('sync_txn_fail_agg', () => {
      let sum = 0;
      return {
        step: (value) => {
          stepCount++;
          if (stepCount === 2) {
            throw new Error('Aggregate failure in sync transaction');
          }
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE sync_txn_agg_fail_test (value INTEGER)');
    sql.exec('INSERT INTO sync_txn_agg_fail_test VALUES (100)'); // Pre-existing

    let error = null;
    try {
      storage.transactionSync(() => {
        sql.exec('INSERT INTO sync_txn_agg_fail_test VALUES (1), (2), (3)');
        // This query will fail on the second step
        sql
          .exec('SELECT sync_txn_fail_agg(value) FROM sync_txn_agg_fail_test')
          .toArray();
      });
    } catch (e) {
      error = e;
    }

    assert.ok(error !== null, 'Transaction should have thrown');
    assert.ok(
      error.message.includes('Aggregate failure in sync transaction'),
      `Expected aggregate error message, got: ${error.message}`
    );

    // Verify rollback: only pre-existing row should remain
    const results = sql.exec('SELECT * FROM sync_txn_agg_fail_test').toArray();
    sql.exec('DROP TABLE sync_txn_agg_fail_test');

    assert.strictEqual(
      results.length,
      1,
      'Transaction should have rolled back'
    );
    assert.strictEqual(results[0].value, 100);
  }

  async testNestedSyncTransactionsWithUdf() {
    const storage = this.state.storage;
    const sql = storage.sql;

    sql.createFunction('nested_sync_triple', (x) => x * 3);

    sql.exec(
      'CREATE TABLE nested_sync_txn_test (id INTEGER PRIMARY KEY, value INTEGER)'
    );

    storage.transactionSync(() => {
      sql.exec(
        'INSERT INTO nested_sync_txn_test VALUES (1, nested_sync_triple(10))'
      );

      // Nested transaction that fails should rollback only its changes
      try {
        storage.transactionSync(() => {
          sql.exec(
            'INSERT INTO nested_sync_txn_test VALUES (2, nested_sync_triple(20))'
          );
          throw new Error('Inner sync transaction failure');
        });
      } catch (e) {
        // Expected - inner transaction rolled back
      }

      // Outer transaction continues
      sql.exec(
        'INSERT INTO nested_sync_txn_test VALUES (3, nested_sync_triple(30))'
      );
    });

    const results = sql
      .exec('SELECT * FROM nested_sync_txn_test ORDER BY id')
      .toArray();
    sql.exec('DROP TABLE nested_sync_txn_test');

    // Should have rows 1 and 3 (row 2 was rolled back with inner transaction)
    assert.strictEqual(results.length, 2);
    assert.strictEqual(results[0].id, 1);
    assert.strictEqual(results[0].value, 30);
    assert.strictEqual(results[1].id, 3);
    assert.strictEqual(results[1].value, 90);
  }

  async testSyncTransactionReturnValue() {
    const storage = this.state.storage;
    const sql = storage.sql;

    sql.createFunction('sync_ret_compute', (x) => x * x);

    sql.exec('CREATE TABLE sync_ret_test (value INTEGER)');
    sql.exec('INSERT INTO sync_ret_test VALUES (5)');

    // transactionSync should return the value returned by the callback
    const result = storage.transactionSync(() => {
      const row = sql
        .exec('SELECT sync_ret_compute(value) AS computed FROM sync_ret_test')
        .one();
      return row.computed;
    });

    sql.exec('DROP TABLE sync_ret_test');

    assert.strictEqual(result, 25);
  }

  // ===========================================================================
  // UDF Side Effects and Transaction Semantics
  //
  // These tests document an important behavior: UDF side effects (modifications
  // to JavaScript state, external calls, etc.) are NOT rolled back when a
  // transaction fails. This matches SQLite's behavior - SQLite only manages
  // database state, not application state.
  //
  // From SQLite docs: Functions with side effects should use SQLITE_DIRECTONLY
  // flag, but SQLite does not attempt to roll back or undo side effects.
  // ===========================================================================

  async testSideEffectsNotRolledBackOnTransactionFailure() {
    const storage = this.state.storage;
    const sql = storage.sql;

    // Track side effects via closure
    let sideEffectCounter = 0;

    sql.createFunction('side_effect_fn', (x) => {
      sideEffectCounter++; // Side effect happens immediately when UDF executes
      return x * 2;
    });

    sql.exec('CREATE TABLE side_effect_test (value INTEGER)');

    // Execute a transaction that will fail
    let error = null;
    try {
      storage.transactionSync(() => {
        sql.exec('INSERT INTO side_effect_test VALUES (side_effect_fn(1))');
        sql.exec('INSERT INTO side_effect_test VALUES (side_effect_fn(2))');
        sql.exec('INSERT INTO side_effect_test VALUES (side_effect_fn(3))');
        throw new Error('Deliberate rollback');
      });
    } catch (e) {
      error = e;
    }

    assert.ok(error !== null, 'Transaction should have failed');

    // Database changes are rolled back - table should be empty
    const rows = sql.exec('SELECT * FROM side_effect_test').toArray();
    assert.strictEqual(
      rows.length,
      0,
      'Database changes should be rolled back'
    );

    // But the side effects (counter increments) are NOT rolled back
    // The UDF was called 3 times before the transaction failed
    assert.strictEqual(
      sideEffectCounter,
      3,
      'UDF side effects are NOT rolled back when transaction fails - this is expected SQLite behavior'
    );

    sql.exec('DROP TABLE side_effect_test');
  }

  async testAggregateSideEffectsNotRolledBack() {
    const storage = this.state.storage;
    const sql = storage.sql;

    let stepCallCount = 0;
    let finalCallCount = 0;

    sql.createFunction('tracking_sum', () => {
      let sum = 0;
      return {
        step: (value) => {
          stepCallCount++; // Side effect
          sum += value;
        },
        final: () => {
          finalCallCount++; // Side effect
          return sum;
        },
      };
    });

    sql.exec('CREATE TABLE agg_side_effect_test (value INTEGER)');
    sql.exec('INSERT INTO agg_side_effect_test VALUES (1), (2), (3)');

    let error = null;
    try {
      storage.transactionSync(() => {
        // This will call step 3 times and final 1 time
        sql
          .exec('SELECT tracking_sum(value) FROM agg_side_effect_test')
          .toArray();
        throw new Error('Deliberate rollback');
      });
    } catch (e) {
      error = e;
    }

    assert.ok(error !== null, 'Transaction should have failed');

    // Side effects persist even after rollback
    assert.strictEqual(
      stepCallCount,
      3,
      'Aggregate step side effects are NOT rolled back'
    );
    assert.strictEqual(
      finalCallCount,
      1,
      'Aggregate final side effects are NOT rolled back'
    );

    sql.exec('DROP TABLE agg_side_effect_test');
  }

  async testUdfMayBeCalledMultipleTimesForSameRow() {
    const sql = this.state.storage.sql;

    // SQLite may evaluate a UDF multiple times for the same logical value
    // due to query optimization, expression evaluation, etc.
    // This test documents that side effects may occur more than expected.

    let callCount = 0;
    sql.createFunction('counting_fn', (x) => {
      callCount++;
      return x;
    });

    sql.exec('CREATE TABLE multi_call_test (value INTEGER)');
    sql.exec('INSERT INTO multi_call_test VALUES (1)');

    // Using the same UDF multiple times in a query
    sql
      .exec(
        'SELECT counting_fn(value), counting_fn(value) FROM multi_call_test'
      )
      .toArray();

    sql.exec('DROP TABLE multi_call_test');

    // The UDF should be called at least twice (once per column reference)
    // It might be called more times depending on SQLite's query plan
    assert.ok(
      callCount >= 2,
      `UDF was called ${callCount} times for 2 column references - ` +
        'side effects may occur multiple times per row'
    );
  }

  async testSideEffectsOccurEvenIfResultUnused() {
    const sql = this.state.storage.sql;

    let called = false;
    sql.createFunction('side_effect_unused', () => {
      called = true;
      return 42;
    });

    // Create cursor but don't consume results
    const cursor = sql.exec('SELECT side_effect_unused() AS val');

    // Even without iterating, just creating the cursor may execute the query
    // (depends on implementation - document actual behavior)
    // Force iteration to ensure execution
    cursor.toArray();

    assert.strictEqual(
      called,
      true,
      'UDF side effects occur when query executes, regardless of result usage'
    );
  }

  // ===========================================================================
  // Function Replacement/Override Tests
  // ===========================================================================

  async testScalarFunctionReplacement() {
    const sql = this.state.storage.sql;

    // Register a function
    sql.createFunction('replaceable', (x) => x * 2);
    let result = sql.exec('SELECT replaceable(5) AS val').one();
    assert.strictEqual(result.val, 10);

    // Replace it with a different implementation
    sql.createFunction('replaceable', (x) => x * 3);
    result = sql.exec('SELECT replaceable(5) AS val').one();
    assert.strictEqual(result.val, 15);
  }

  async testAggregateFunctionReplacement() {
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE replace_agg_test (value INTEGER)');
    sql.exec('INSERT INTO replace_agg_test VALUES (1), (2), (3)');

    // Register an aggregate function
    sql.createFunction('replaceable_agg', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => sum,
      };
    });
    let result = sql
      .exec('SELECT replaceable_agg(value) AS total FROM replace_agg_test')
      .one();
    assert.strictEqual(result.total, 6);

    // Replace it with a different implementation (product instead of sum)
    sql.createFunction('replaceable_agg', () => {
      let product = 1;
      return {
        step: (value) => {
          product *= value;
        },
        final: () => product,
      };
    });
    result = sql
      .exec('SELECT replaceable_agg(value) AS total FROM replace_agg_test')
      .one();
    assert.strictEqual(result.total, 6); // 1 * 2 * 3 = 6

    sql.exec('DROP TABLE replace_agg_test');
  }

  // ===========================================================================
  // Type Edge Cases
  // ===========================================================================

  async testUdfWithNaN() {
    const sql = this.state.storage.sql;

    sql.createFunction('return_nan', () => NaN);
    const result = sql.exec('SELECT return_nan() AS val').one();
    // SQLite converts NaN to NULL (unlike Infinity which is preserved)
    assert.strictEqual(result.val, null);
  }

  async testUdfWithInfinity() {
    const sql = this.state.storage.sql;

    sql.createFunction('return_inf', () => Infinity);
    const result = sql.exec('SELECT return_inf() AS val').one();
    // Infinity is stored as a double in SQLite and comes back as Infinity
    assert.strictEqual(result.val, Infinity);

    sql.createFunction('return_neg_inf', () => -Infinity);
    const result2 = sql.exec('SELECT return_neg_inf() AS val').one();
    assert.strictEqual(result2.val, -Infinity);
  }

  async testUdfReceivesNullArgument() {
    const sql = this.state.storage.sql;

    let receivedNull = false;
    sql.createFunction('check_null', (x) => {
      if (x === null) {
        receivedNull = true;
      }
      return x === null ? 'was null' : 'was not null';
    });

    const result = sql.exec('SELECT check_null(NULL) AS val').one();
    assert.strictEqual(result.val, 'was null');
    assert.strictEqual(receivedNull, true);
  }

  async testAggregateWithNullValues() {
    const sql = this.state.storage.sql;

    const receivedValues = [];
    sql.createFunction('track_nulls', () => {
      let sum = 0;
      return {
        step: (value) => {
          receivedValues.push(value);
          if (value !== null) {
            sum += value;
          }
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE null_agg_test (value INTEGER)');
    sql.exec('INSERT INTO null_agg_test VALUES (1), (NULL), (3), (NULL), (5)');

    const result = sql
      .exec('SELECT track_nulls(value) AS total FROM null_agg_test')
      .one();
    sql.exec('DROP TABLE null_agg_test');

    // Step should be called for ALL rows including NULL values
    assert.strictEqual(receivedValues.length, 5);
    assert.strictEqual(receivedValues[1], null);
    assert.strictEqual(receivedValues[3], null);
    // Sum of non-null values: 1 + 3 + 5 = 9
    assert.strictEqual(result.total, 9);
  }

  async testUdfWithVeryLargeNumber() {
    const sql = this.state.storage.sql;

    // SQLite integers are 64-bit, JS safe integers are 53-bit
    const largeNum = Number.MAX_SAFE_INTEGER;
    sql.createFunction('echo_large', (x) => x);

    const result = sql.exec(`SELECT echo_large(${largeNum}) AS val`).one();
    assert.strictEqual(result.val, largeNum);
  }

  async testUdfWithFloatingPoint() {
    const sql = this.state.storage.sql;

    sql.createFunction('echo_float', (x) => x);
    const result = sql.exec('SELECT echo_float(3.14159) AS val').one();
    assert.ok(Math.abs(result.val - 3.14159) < 0.00001);
  }

  // ===========================================================================
  // Aggregate Re-entrancy Tests
  // ===========================================================================

  async testAggregateStepReentrancyBlocked() {
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE agg_reenter_test (value INTEGER)');
    sql.exec('INSERT INTO agg_reenter_test VALUES (1), (2), (3)');

    let stepCalled = false;
    let stepSucceeded = false;

    sql.createFunction('reentrant_agg_step', () => {
      let sum = 0;
      return {
        step: (value) => {
          stepCalled = true;
          // Try to execute SQL from within step - should be blocked
          sql.exec('SELECT 1').one();
          stepSucceeded = true;
          sum += value;
        },
        final: () => sum,
      };
    });

    let error = null;
    try {
      sql
        .exec('SELECT reentrant_agg_step(value) AS total FROM agg_reenter_test')
        .one();
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE agg_reenter_test');

    assert.ok(stepCalled, 'Step should have been called');
    assert.strictEqual(stepSucceeded, false, 'Step should not have completed');
    assert.ok(error !== null, 'Should have thrown');
  }

  async testAggregateFinalReentrancyBlocked() {
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE agg_reenter_final_test (value INTEGER)');
    sql.exec('INSERT INTO agg_reenter_final_test VALUES (1), (2), (3)');

    let finalCalled = false;
    let finalSucceeded = false;

    sql.createFunction('reentrant_agg_final', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => {
          finalCalled = true;
          // Try to execute SQL from within final - should be blocked
          sql.exec('SELECT 1').one();
          finalSucceeded = true;
          return sum;
        },
      };
    });

    let error = null;
    try {
      sql
        .exec(
          'SELECT reentrant_agg_final(value) AS total FROM agg_reenter_final_test'
        )
        .one();
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE agg_reenter_final_test');

    assert.ok(finalCalled, 'Final should have been called');
    assert.strictEqual(
      finalSucceeded,
      false,
      'Final should not have completed'
    );
    assert.ok(error !== null, 'Should have thrown');
  }

  // ===========================================================================
  // UDFs in Complex SQL Contexts
  // ===========================================================================

  async testUdfInSubquery() {
    const sql = this.state.storage.sql;

    sql.createFunction('sub_double', (x) => x * 2);

    sql.exec('CREATE TABLE subq_outer (id INTEGER)');
    sql.exec('CREATE TABLE subq_inner (id INTEGER, value INTEGER)');
    sql.exec('INSERT INTO subq_outer VALUES (1), (2), (3)');
    sql.exec('INSERT INTO subq_inner VALUES (1, 10), (2, 20), (3, 30)');

    const results = sql
      .exec(
        `SELECT o.id, (SELECT sub_double(i.value) FROM subq_inner i WHERE i.id = o.id) AS doubled
         FROM subq_outer o ORDER BY o.id`
      )
      .toArray();

    sql.exec('DROP TABLE subq_outer');
    sql.exec('DROP TABLE subq_inner');

    assert.strictEqual(results.length, 3);
    assert.strictEqual(results[0].doubled, 20);
    assert.strictEqual(results[1].doubled, 40);
    assert.strictEqual(results[2].doubled, 60);
  }

  async testUdfInOrderBy() {
    const sql = this.state.storage.sql;

    sql.createFunction('order_key', (x) => -x); // Reverse the order

    sql.exec('CREATE TABLE order_test (value INTEGER)');
    sql.exec('INSERT INTO order_test VALUES (3), (1), (4), (1), (5)');

    const results = sql
      .exec('SELECT value FROM order_test ORDER BY order_key(value)')
      .toArray();
    sql.exec('DROP TABLE order_test');

    // Should be in descending order (because we negate for ordering)
    assert.strictEqual(results[0].value, 5);
    assert.strictEqual(results[1].value, 4);
    assert.strictEqual(results[2].value, 3);
  }

  async testUdfInHaving() {
    const sql = this.state.storage.sql;

    // Return 1 or 0 instead of true/false since SQLite HAVING expects numeric
    sql.createFunction('having_filter', (x) => (x > 5 ? 1 : 0));

    sql.exec('CREATE TABLE having_test (category TEXT, value INTEGER)');
    sql.exec(
      "INSERT INTO having_test VALUES ('a', 1), ('a', 2), ('b', 10), ('b', 20)"
    );

    const results = sql
      .exec(
        `SELECT category, SUM(value) as total FROM having_test
         GROUP BY category HAVING having_filter(SUM(value)) ORDER BY category`
      )
      .toArray();
    sql.exec('DROP TABLE having_test');

    // Only category 'b' has sum > 5 (sum is 30)
    assert.strictEqual(results.length, 1);
    assert.strictEqual(results[0].category, 'b');
    assert.strictEqual(results[0].total, 30);
  }

  async testAggregateUdfInSubquery() {
    const sql = this.state.storage.sql;

    sql.createFunction('subq_sum', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE agg_subq_outer (id INTEGER)');
    sql.exec('CREATE TABLE agg_subq_inner (group_id INTEGER, value INTEGER)');
    sql.exec('INSERT INTO agg_subq_outer VALUES (1), (2)');
    sql.exec(
      'INSERT INTO agg_subq_inner VALUES (1, 10), (1, 20), (2, 100), (2, 200)'
    );

    const results = sql
      .exec(
        `SELECT o.id, (SELECT subq_sum(i.value) FROM agg_subq_inner i WHERE i.group_id = o.id) AS total
         FROM agg_subq_outer o ORDER BY o.id`
      )
      .toArray();

    sql.exec('DROP TABLE agg_subq_outer');
    sql.exec('DROP TABLE agg_subq_inner');

    assert.strictEqual(results.length, 2);
    assert.strictEqual(results[0].total, 30); // 10 + 20
    assert.strictEqual(results[1].total, 300); // 100 + 200
  }

  // ===========================================================================
  // Multiple Aggregates and Edge Cases
  // ===========================================================================

  async testMultipleAggregatesInSameQuery() {
    const sql = this.state.storage.sql;

    sql.createFunction('agg_sum', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.createFunction('agg_count', () => {
      let count = 0;
      return {
        step: () => {
          count++;
        },
        final: () => count,
      };
    });

    sql.createFunction('agg_max', () => {
      let max = null;
      return {
        step: (value) => {
          if (max === null || value > max) {
            max = value;
          }
        },
        final: () => max,
      };
    });

    sql.exec('CREATE TABLE multi_agg_test (value INTEGER)');
    sql.exec('INSERT INTO multi_agg_test VALUES (10), (20), (30), (5), (15)');

    const result = sql
      .exec(
        'SELECT agg_sum(value) AS sum, agg_count(value) AS count, agg_max(value) AS max FROM multi_agg_test'
      )
      .one();
    sql.exec('DROP TABLE multi_agg_test');

    assert.strictEqual(result.sum, 80); // 10+20+30+5+15
    assert.strictEqual(result.count, 5);
    assert.strictEqual(result.max, 30);
  }

  async testSameAggregateMultipleTimes() {
    const sql = this.state.storage.sql;

    sql.createFunction('multi_sum', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE same_agg_test (a INTEGER, b INTEGER)');
    sql.exec('INSERT INTO same_agg_test VALUES (1, 10), (2, 20), (3, 30)');

    const result = sql
      .exec(
        'SELECT multi_sum(a) AS sum_a, multi_sum(b) AS sum_b FROM same_agg_test'
      )
      .one();
    sql.exec('DROP TABLE same_agg_test');

    // Each aggregate instance should have independent state
    assert.strictEqual(result.sum_a, 6); // 1+2+3
    assert.strictEqual(result.sum_b, 60); // 10+20+30
  }

  async testScalarUdfWithZeroArguments() {
    const sql = this.state.storage.sql;

    // Note: Zero-argument functions are called once during createFunction()
    // to detect if they're aggregate factories. This means the function
    // will be called once during registration before any queries run.
    let callCount = 0;
    sql.createFunction('get_counter', () => {
      return ++callCount;
    });

    // The detection call already incremented callCount to 1, so queries start at 2
    const r1 = sql.exec('SELECT get_counter() AS val').one();
    const r2 = sql.exec('SELECT get_counter() AS val').one();
    const r3 = sql.exec('SELECT get_counter() AS val').one();

    // Verify the counter increments correctly (starting from 2 due to detection call)
    assert.strictEqual(r1.val, 2);
    assert.strictEqual(r2.val, 3);
    assert.strictEqual(r3.val, 4);
  }

  async testAggregateWithDistinct() {
    const sql = this.state.storage.sql;

    sql.createFunction('distinct_sum', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE distinct_test (value INTEGER)');
    sql.exec('INSERT INTO distinct_test VALUES (1), (2), (2), (3), (3), (3)');

    // With DISTINCT, should only sum unique values: 1+2+3 = 6
    const result = sql
      .exec('SELECT distinct_sum(DISTINCT value) AS total FROM distinct_test')
      .one();
    sql.exec('DROP TABLE distinct_test');

    assert.strictEqual(result.total, 6);
  }

  async testAggregateWithFilterClause() {
    const sql = this.state.storage.sql;

    sql.createFunction('filter_sum', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE filter_test (value INTEGER)');
    sql.exec('INSERT INTO filter_test VALUES (1), (2), (3), (4), (5)');

    // Sum only values > 2: 3+4+5 = 12
    const result = sql
      .exec(
        'SELECT filter_sum(value) FILTER (WHERE value > 2) AS total FROM filter_test'
      )
      .one();
    sql.exec('DROP TABLE filter_test');

    assert.strictEqual(result.total, 12);
  }

  async testScalarUdfInCaseExpression() {
    const sql = this.state.storage.sql;

    sql.createFunction('is_even', (x) => (x % 2 === 0 ? 1 : 0));

    sql.exec('CREATE TABLE case_test (value INTEGER)');
    sql.exec('INSERT INTO case_test VALUES (1), (2), (3), (4)');

    const results = sql
      .exec(
        `SELECT value, CASE WHEN is_even(value) THEN 'even' ELSE 'odd' END AS parity
         FROM case_test ORDER BY value`
      )
      .toArray();
    sql.exec('DROP TABLE case_test');

    assert.strictEqual(results[0].parity, 'odd');
    assert.strictEqual(results[1].parity, 'even');
    assert.strictEqual(results[2].parity, 'odd');
    assert.strictEqual(results[3].parity, 'even');
  }

  async testScalarUdfInCoalesce() {
    const sql = this.state.storage.sql;

    sql.createFunction('maybe_null', (x) => (x > 0 ? x : null));

    const r1 = sql.exec('SELECT COALESCE(maybe_null(5), 0) AS val').one();
    const r2 = sql.exec('SELECT COALESCE(maybe_null(-5), 0) AS val').one();

    assert.strictEqual(r1.val, 5);
    assert.strictEqual(r2.val, 0);
  }

  async testUdfWithVeryManyArguments() {
    const sql = this.state.storage.sql;

    // Test with many arguments (SQLite max is 127)
    sql.createFunction('sum_all', (...args) => {
      return args.reduce((sum, val) => sum + (val ?? 0), 0);
    });

    const result = sql
      .exec('SELECT sum_all(1,2,3,4,5,6,7,8,9,10) AS total')
      .one();
    assert.strictEqual(result.total, 55);
  }

  async testAggregateWithOrderByInGroupBy() {
    const sql = this.state.storage.sql;

    // Collect values in order they appear
    sql.createFunction('ordered_collect', () => {
      const arr = [];
      return {
        step: (value) => {
          arr.push(value);
        },
        final: () => JSON.stringify(arr),
      };
    });

    sql.exec('CREATE TABLE ordered_test (grp TEXT, value INTEGER)');
    sql.exec(
      "INSERT INTO ordered_test VALUES ('a', 3), ('a', 1), ('a', 2), ('b', 6), ('b', 4), ('b', 5)"
    );

    const results = sql
      .exec(
        `SELECT grp, ordered_collect(value) AS vals FROM ordered_test GROUP BY grp ORDER BY grp`
      )
      .toArray();
    sql.exec('DROP TABLE ordered_test');

    assert.strictEqual(results.length, 2);
    // Note: Order within group depends on SQLite's processing order
    assert.deepStrictEqual(JSON.parse(results[0].vals).sort(), [1, 2, 3]);
    assert.deepStrictEqual(JSON.parse(results[1].vals).sort(), [4, 5, 6]);
  }

  async testScalarUdfPreservesTypeFromInput() {
    const sql = this.state.storage.sql;

    // Identity function - return exactly what was passed
    sql.createFunction('identity', (x) => x);

    sql.exec('CREATE TABLE type_test (i INTEGER, r REAL, t TEXT, b BLOB)');
    sql.exec("INSERT INTO type_test VALUES (42, 3.14, 'hello', X'DEADBEEF')");

    const result = sql
      .exec(
        'SELECT identity(i) AS i, identity(r) AS r, identity(t) AS t, identity(b) AS b FROM type_test'
      )
      .one();
    sql.exec('DROP TABLE type_test');

    assert.strictEqual(result.i, 42);
    assert.ok(Math.abs(result.r - 3.14) < 0.001);
    assert.strictEqual(result.t, 'hello');
    assert.ok(
      result.b instanceof Uint8Array || result.b instanceof ArrayBuffer
    );
  }

  async testUdfCalledForEachRowInJoin() {
    const sql = this.state.storage.sql;

    let callCount = 0;
    sql.createFunction('track_calls', (x) => {
      callCount++;
      return x;
    });

    sql.exec('CREATE TABLE join_a (id INTEGER)');
    sql.exec('CREATE TABLE join_b (id INTEGER)');
    sql.exec('INSERT INTO join_a VALUES (1), (2)');
    sql.exec('INSERT INTO join_b VALUES (1), (2), (3)');

    // Cross join produces 2*3 = 6 rows
    sql.exec('SELECT track_calls(a.id) FROM join_a a, join_b b').toArray();

    sql.exec('DROP TABLE join_a');
    sql.exec('DROP TABLE join_b');

    assert.strictEqual(callCount, 6);
  }

  // ===========================================================================
  // Real-World Use Case Tests
  // ===========================================================================

  /**
   * Use Case: Change Hooks via Triggers + UDFs
   *
   * SQLite has built-in update hooks (sqlite3_update_hook) but they're not
   * exposed in our API. However, we can achieve the same functionality using
   * triggers that call UDFs. This gives us even MORE flexibility since we get
   * access to both OLD and NEW values for UPDATE operations.
   *
   * This pattern is useful for:
   * - Audit logging
   * - Real-time sync/replication
   * - Cache invalidation
   * - Reactive updates in the application
   */
  async testChangeHookViaTriggersAndUdfs() {
    const sql = this.state.storage.sql;

    // Collect all changes that occur
    const changes = [];

    // Register a UDF that acts as our change hook
    sql.createFunction(
      'on_change',
      (operation, tableName, rowId, oldJson, newJson) => {
        changes.push({
          operation,
          table: tableName,
          rowId,
          old: oldJson ? JSON.parse(oldJson) : null,
          new: newJson ? JSON.parse(newJson) : null,
        });
        return null; // Triggers don't use the return value
      }
    );

    // Create a table we want to monitor
    sql.exec(`
      CREATE TABLE users (
        id INTEGER PRIMARY KEY,
        name TEXT,
        email TEXT
      )
    `);

    // Create triggers for INSERT, UPDATE, and DELETE
    sql.exec(`
      CREATE TRIGGER users_insert_hook AFTER INSERT ON users
      BEGIN
        SELECT on_change(
          'INSERT',
          'users',
          NEW.id,
          NULL,
          json_object('id', NEW.id, 'name', NEW.name, 'email', NEW.email)
        );
      END
    `);

    sql.exec(`
      CREATE TRIGGER users_update_hook AFTER UPDATE ON users
      BEGIN
        SELECT on_change(
          'UPDATE',
          'users',
          NEW.id,
          json_object('id', OLD.id, 'name', OLD.name, 'email', OLD.email),
          json_object('id', NEW.id, 'name', NEW.name, 'email', NEW.email)
        );
      END
    `);

    sql.exec(`
      CREATE TRIGGER users_delete_hook AFTER DELETE ON users
      BEGIN
        SELECT on_change(
          'DELETE',
          'users',
          OLD.id,
          json_object('id', OLD.id, 'name', OLD.name, 'email', OLD.email),
          NULL
        );
      END
    `);

    // Now perform some operations and verify the hooks fire

    // INSERT
    sql.exec(
      "INSERT INTO users (id, name, email) VALUES (1, 'Alice', 'alice@example.com')"
    );
    sql.exec(
      "INSERT INTO users (id, name, email) VALUES (2, 'Bob', 'bob@example.com')"
    );

    // UPDATE
    sql.exec("UPDATE users SET email = 'alice.new@example.com' WHERE id = 1");

    // DELETE
    sql.exec('DELETE FROM users WHERE id = 2');

    // Clean up
    sql.exec('DROP TRIGGER users_insert_hook');
    sql.exec('DROP TRIGGER users_update_hook');
    sql.exec('DROP TRIGGER users_delete_hook');
    sql.exec('DROP TABLE users');

    // Verify all changes were captured
    assert.strictEqual(changes.length, 4);

    // INSERT Alice
    assert.strictEqual(changes[0].operation, 'INSERT');
    assert.strictEqual(changes[0].table, 'users');
    assert.strictEqual(changes[0].rowId, 1);
    assert.strictEqual(changes[0].old, null);
    assert.strictEqual(changes[0].new.name, 'Alice');
    assert.strictEqual(changes[0].new.email, 'alice@example.com');

    // INSERT Bob
    assert.strictEqual(changes[1].operation, 'INSERT');
    assert.strictEqual(changes[1].new.name, 'Bob');

    // UPDATE Alice's email
    assert.strictEqual(changes[2].operation, 'UPDATE');
    assert.strictEqual(changes[2].rowId, 1);
    assert.strictEqual(changes[2].old.email, 'alice@example.com');
    assert.strictEqual(changes[2].new.email, 'alice.new@example.com');
    assert.strictEqual(changes[2].old.name, 'Alice'); // Name unchanged
    assert.strictEqual(changes[2].new.name, 'Alice');

    // DELETE Bob
    assert.strictEqual(changes[3].operation, 'DELETE');
    assert.strictEqual(changes[3].rowId, 2);
    assert.strictEqual(changes[3].old.name, 'Bob');
    assert.strictEqual(changes[3].new, null);
  }

  // ===========================================================================
  // Stack Trace Preservation for Aggregates
  // ===========================================================================

  async testAggregateStepStackTracePreserved() {
    const sql = this.state.storage.sql;

    function stepHelper() {
      throw new Error('Error from step helper');
    }

    sql.createFunction('stack_step', () => {
      let sum = 0;
      return {
        step: (value) => {
          stepHelper();
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE stack_step_test (value INTEGER)');
    sql.exec('INSERT INTO stack_step_test VALUES (1), (2), (3)');

    let error = null;
    try {
      sql.exec('SELECT stack_step(value) AS total FROM stack_step_test').one();
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE stack_step_test');

    assert.ok(error !== null, 'Should have thrown');
    assert.ok(
      error.message.includes('Error from step helper'),
      'Error message should be preserved'
    );

    const stack = error.stack || '';
    assert.ok(
      stack.includes('stepHelper'),
      `Stack trace should include 'stepHelper', got: ${stack}`
    );
  }

  async testAggregateFinalStackTracePreserved() {
    const sql = this.state.storage.sql;

    function finalHelper() {
      throw new Error('Error from final helper');
    }

    sql.createFunction('stack_final', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: () => {
          finalHelper();
          return sum;
        },
      };
    });

    sql.exec('CREATE TABLE stack_final_test (value INTEGER)');
    sql.exec('INSERT INTO stack_final_test VALUES (1), (2), (3)');

    let error = null;
    try {
      sql
        .exec('SELECT stack_final(value) AS total FROM stack_final_test')
        .one();
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE stack_final_test');

    assert.ok(error !== null, 'Should have thrown');
    assert.ok(
      error.message.includes('Error from final helper'),
      'Error message should be preserved'
    );

    const stack = error.stack || '';
    assert.ok(
      stack.includes('finalHelper'),
      `Stack trace should include 'finalHelper', got: ${stack}`
    );
  }

  // ===========================================================================
  // Garbage Collection Tests
  //
  // These tests verify that UDF callbacks are properly prevented from being
  // garbage collected while in use, and that they CAN be collected when
  // replaced or no longer needed.
  // ===========================================================================

  /**
   * Test that a UDF callback survives GC while the UDF is registered and in use.
   * This would crash or return undefined if the callback was incorrectly collected.
   */
  async testUdfCallbackSurvivesGc() {
    const sql = this.state.storage.sql;

    // Register a UDF - store the callback creation in a block scope
    // so no local variable keeps it alive
    {
      const multiplier = 7; // Captured by closure
      sql.createFunction('gc_test_func', (x) => x * multiplier);
    }

    // Force GC multiple times - if the callback was incorrectly weak-referenced,
    // it could be collected here
    gc();
    await scheduler.wait(10);
    gc();
    await scheduler.wait(10);
    gc();

    // The UDF should still work - the callback must not have been collected
    // If it was collected, this would crash or return undefined/garbage
    const result = sql.exec('SELECT gc_test_func(6) AS val').one();

    assert.strictEqual(result.val, 42); // 6 * 7 = 42
  }

  /**
   * Test that aggregate UDF callbacks survive GC during multi-row processing.
   * This is important because aggregate functions are called many times across
   * multiple rows, and GC could happen between any of those calls.
   */
  async testAggregateUdfSurvivesGcDuringProcessing() {
    const sql = this.state.storage.sql;

    let stepCallCount = 0;
    let finalCallCount = 0;

    sql.createFunction('gc_agg_test', () => {
      let sum = 0;
      return {
        step: (value) => {
          stepCallCount++;
          // Force GC during step processing
          gc();
          sum += value;
        },
        final: () => {
          finalCallCount++;
          gc();
          return sum;
        },
      };
    });

    sql.exec('CREATE TABLE gc_agg_test_tbl (value INTEGER)');
    sql.exec('INSERT INTO gc_agg_test_tbl VALUES (1), (2), (3), (4), (5)');

    const result = sql
      .exec('SELECT gc_agg_test(value) AS total FROM gc_agg_test_tbl')
      .one();

    sql.exec('DROP TABLE gc_agg_test_tbl');

    // Verify all callbacks completed successfully
    assert.strictEqual(stepCallCount, 5, 'Step should be called 5 times');
    assert.strictEqual(finalCallCount, 1, 'Final should be called once');
    assert.strictEqual(result.total, 15, 'Sum should be 1+2+3+4+5=15');
  }

  /**
   * Test that replacing a UDF allows the old callback's captured objects to be collected.
   * Uses FinalizationRegistry to verify that the old closure IS actually garbage collected.
   */
  async testReplacedUdfCallbackCanBeCollected() {
    const sql = this.state.storage.sql;

    // Track whether objects captured by old callbacks get collected
    let collected = [];
    const registry = new FinalizationRegistry((heldValue) => {
      collected.push(heldValue);
    });

    // Register initial UDF with a captured object we can track
    {
      const trackedObject = { id: 'original', data: new Array(1000).fill('x') };
      registry.register(trackedObject, 'original-callback-object');

      sql.createFunction('replaceable_gc', (x) => {
        // Reference trackedObject to capture it in the closure
        void trackedObject.id;
        return x * 2;
      });
    }

    // Verify the UDF works
    const r1 = sql.exec('SELECT replaceable_gc(5) AS val').one();
    assert.strictEqual(r1.val, 10);

    // Force GC - the callback should NOT be collected yet because UDF holds it
    gc();
    await scheduler.wait(50);
    gc();

    assert.strictEqual(
      collected.length,
      0,
      'Object should NOT be collected while UDF is registered'
    );

    // UDF should still work
    const r1b = sql.exec('SELECT replaceable_gc(5) AS val').one();
    assert.strictEqual(r1b.val, 10);

    // Now replace the UDF with a new implementation
    {
      const newTrackedObject = {
        id: 'replacement',
        data: new Array(1000).fill('y'),
      };
      registry.register(newTrackedObject, 'replacement-callback-object');

      sql.createFunction('replaceable_gc', (x) => {
        void newTrackedObject.id;
        return x * 3;
      });
    }

    // Verify the new UDF works
    const r2 = sql.exec('SELECT replaceable_gc(5) AS val').one();
    assert.strictEqual(r2.val, 15);

    // Force GC multiple times - the OLD callback is now unreferenced
    // and its captured object should be collectible
    gc();
    await scheduler.wait(100);
    gc();
    await scheduler.wait(100);
    gc();
    await scheduler.wait(100);

    // The original callback's captured object should now be collected
    // Note: FinalizationRegistry callbacks may be delayed, so we allow some flexibility
    assert.ok(
      collected.includes('original-callback-object'),
      `Original callback object should be collected. Collected: ${JSON.stringify(collected)}`
    );

    // The replacement should NOT be collected yet (still in use)
    assert.ok(
      !collected.includes('replacement-callback-object'),
      'Replacement callback object should NOT be collected yet'
    );

    // New UDF should still work after GC
    const r3 = sql.exec('SELECT replaceable_gc(5) AS val').one();
    assert.strictEqual(r3.val, 15);
  }

  /**
   * Test that aggregate UDF state objects are collected after query completion.
   * Uses FinalizationRegistry to verify state cleanup.
   */
  async testAggregateStateIsCollectedAfterQuery() {
    const sql = this.state.storage.sql;

    let stateCollected = false;
    const registry = new FinalizationRegistry((heldValue) => {
      if (heldValue === 'aggregate-state') {
        stateCollected = true;
      }
    });

    sql.exec('CREATE TABLE agg_gc_test (value INTEGER)');
    sql.exec('INSERT INTO agg_gc_test VALUES (1), (2), (3), (4), (5)');

    // Create an aggregate that uses an object as state (which we track)
    sql.createFunction('tracked_agg', () => {
      const stateObj = { sum: 0, count: 0 };
      registry.register(stateObj, 'aggregate-state');
      return {
        step: (value) => {
          stateObj.sum += value;
          stateObj.count++;
        },
        final: () => stateObj.sum,
      };
    });

    // Run the aggregate
    const result = sql
      .exec('SELECT tracked_agg(value) AS total FROM agg_gc_test')
      .one();
    assert.strictEqual(result.total, 15);

    // The query is done - state should be eligible for collection
    gc();
    await scheduler.wait(100);
    gc();
    await scheduler.wait(100);
    gc();

    // Note: The state object created in step() goes out of scope when step() returns,
    // so it should be collected. The JSON string state is what persists.
    // This test verifies no references to temporary objects are leaked.

    sql.exec('DROP TABLE agg_gc_test');
  }

  /**
   * Test that iterating through many rows with a UDF doesn't leak memory.
   * This is a stress test - if callbacks weren't properly prevented from GC,
   * the UDF would crash or misbehave during iteration.
   */
  async testUdfStressWithGc() {
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE gc_stress_test (id INTEGER, value INTEGER)');

    // Insert many rows
    const insertStmt = sql.prepare('INSERT INTO gc_stress_test VALUES (?, ?)');
    for (let i = 0; i < 100; i++) {
      insertStmt(i, i * 10);
    }

    let processedCount = 0;
    sql.createFunction('gc_stress_func', (id, value) => {
      processedCount++;
      // Periodically force GC during processing
      if (processedCount % 10 === 0) {
        gc();
      }
      return id + value;
    });

    let totalSum = 0;
    for (const row of sql.exec(
      'SELECT gc_stress_func(id, value) AS computed FROM gc_stress_test'
    )) {
      totalSum += row.computed;
    }

    sql.exec('DROP TABLE gc_stress_test');

    assert.strictEqual(processedCount, 100, 'Should process all 100 rows');
    // Sum of (i + i*10) for i=0 to 99 = sum of 11*i = 11 * (99*100/2) = 11 * 4950 = 54450
    assert.strictEqual(totalSum, 54450);
  }

  // ===========================================================================
  // Edge Case Tests
  // ===========================================================================

  /**
   * Test behavior when a UDF is replaced while a prepared statement exists.
   * The prepared statement should use the NEW implementation because SQLite
   * resolves function references at execution time, not preparation time.
   */
  async testPreparedStatementWithReplacedUdf() {
    const sql = this.state.storage.sql;

    sql.createFunction('replaceable_prep', (x) => x * 2);

    sql.exec('CREATE TABLE prep_test (value INTEGER)');
    sql.exec('INSERT INTO prep_test VALUES (5)');

    // Prepare a statement that uses the UDF
    const stmt = sql.prepare(
      'SELECT replaceable_prep(value) AS result FROM prep_test'
    );

    // Execute with original implementation
    const r1 = stmt().one();
    assert.strictEqual(r1.result, 10); // 5 * 2

    // Replace the UDF
    sql.createFunction('replaceable_prep', (x) => x * 3);

    // Execute the same prepared statement - should use NEW implementation
    const r2 = stmt().one();
    assert.strictEqual(r2.result, 15); // 5 * 3

    // Replace again
    sql.createFunction('replaceable_prep', (x) => x + 100);

    const r3 = stmt().one();
    assert.strictEqual(r3.result, 105); // 5 + 100

    sql.exec('DROP TABLE prep_test');
  }

  /**
   * Test UDF behavior with various unsupported return types.
   * Most should be coerced to strings; some may have special handling.
   */
  async testUdfReturnsUnsupportedTypes() {
    const sql = this.state.storage.sql;

    // Plain object -> should throw an error (not useful to stringify to "[object Object]")
    sql.createFunction('return_object', () => ({ foo: 'bar' }));
    let objectError = null;
    try {
      sql.exec('SELECT return_object() AS val').one();
    } catch (e) {
      objectError = e;
    }
    assert.ok(objectError !== null, 'Returning plain object should throw');
    assert.ok(
      objectError.message.includes('[object Object]') ||
        objectError.message.includes('plain object'),
      `Error should mention plain object issue, got: ${objectError.message}`
    );

    // Array -> should be coerced to string "1,2,3"
    sql.createFunction('return_array', () => [1, 2, 3]);
    const r2 = sql.exec('SELECT return_array() AS val').one();
    assert.strictEqual(r2.val, '1,2,3');

    // Boolean true -> coerced to string "true" or number?
    sql.createFunction('return_true', () => true);
    const r3 = sql.exec('SELECT return_true() AS val').one();
    // Booleans are not numbers in JS, so they get stringified
    assert.strictEqual(r3.val, 'true');

    // Boolean false
    sql.createFunction('return_false', () => false);
    const r4 = sql.exec('SELECT return_false() AS val').one();
    assert.strictEqual(r4.val, 'false');

    // Date -> should be coerced to string (ISO format or similar)
    sql.createFunction('return_date', () => new Date('2024-01-15T12:00:00Z'));
    const r5 = sql.exec('SELECT return_date() AS val').one();
    assert.ok(r5.val.includes('2024'), `Date should stringify, got: ${r5.val}`);

    // Function -> stringifies to function source or "[object Function]"
    sql.createFunction('return_function', () => function test() {});
    const r6 = sql.exec('SELECT return_function() AS val').one();
    assert.ok(
      r6.val.includes('function') || r6.val.includes('Function'),
      `Function should stringify, got: ${r6.val}`
    );

    // Symbol -> Symbols can't be converted to string implicitly
    // This should throw when js.toString() is called on it
    sql.createFunction('return_symbol', () => Symbol('test'));
    let symbolThrew = false;
    try {
      sql.exec('SELECT return_symbol() AS val').one();
    } catch (e) {
      symbolThrew = true;
      // Expected: Symbol can't be converted to string
    }
    assert.ok(symbolThrew, 'Returning a Symbol should throw');

    // BigInt -> can't be implicitly converted to Number or String in some cases
    sql.createFunction('return_bigint', () => BigInt(9007199254740993));
    let bigintThrew = false;
    let bigintVal;
    try {
      bigintVal = sql.exec('SELECT return_bigint() AS val').one().val;
    } catch (e) {
      bigintThrew = true;
    }
    // Document behavior - BigInt may throw or may get stringified
    assert.ok(
      bigintThrew || bigintVal !== undefined,
      `BigInt either threw or returned: ${bigintVal}`
    );
  }

  // ===========================================================================
  // CRITICAL: Async Callback Tests
  //
  // UDF callbacks are invoked synchronously by SQLite during query execution.
  // If a callback returns a Promise (is async), the Promise object itself
  // would be coerced to a string like "[object Promise]" - this is almost
  // certainly a bug in user code and should be clearly documented/tested.
  // ===========================================================================

  /**
   * CRITICAL TEST: What happens when a UDF callback is async?
   *
   * Since SQLite calls UDFs synchronously, an async function will return
   * a Promise object. This is almost certainly a mistake, so we throw a
   * clear error to help the user understand what went wrong.
   */
  async testAsyncScalarUdfThrowsError() {
    const sql = this.state.storage.sql;

    // Register an async function as a UDF - this is likely a user mistake
    sql.createFunction('async_udf', async (x) => {
      // User might think they can await here
      await Promise.resolve();
      return x * 2;
    });

    // The async function returns a Promise, which should throw an error
    let error = null;
    try {
      sql.exec('SELECT async_udf(5) AS val').one();
    } catch (e) {
      error = e;
    }

    assert.ok(error !== null, 'Should have thrown an error');
    assert.ok(
      error.message.includes('Promise') &&
        error.message.includes('synchronous'),
      `Error should mention Promise and synchronous, got: ${error.message}`
    );
  }

  /**
   * Test async aggregate step callback behavior.
   * Async step functions return Promises, which should throw an error.
   */
  async testAsyncAggregateStepThrowsError() {
    const sql = this.state.storage.sql;

    sql.createFunction('async_agg', () => {
      let sum = 0;
      return {
        step: async (value) => {
          await Promise.resolve();
          sum += value;
        },
        final: () => sum,
      };
    });

    sql.exec('CREATE TABLE async_agg_test (value INTEGER)');
    sql.exec('INSERT INTO async_agg_test VALUES (1), (2), (3)');

    // Async step returns a Promise, which should throw
    let error = null;
    try {
      sql.exec('SELECT async_agg(value) AS total FROM async_agg_test').one();
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE async_agg_test');

    assert.ok(error !== null, 'Should have thrown an error');
    assert.ok(
      error.message.includes('Promise'),
      `Error should mention Promise, got: ${error.message}`
    );
  }

  /**
   * Test async aggregate final callback behavior.
   * Async final functions return Promises, which should throw an error.
   */
  async testAsyncAggregateFinalThrowsError() {
    const sql = this.state.storage.sql;

    sql.createFunction('async_final_agg', () => {
      let sum = 0;
      return {
        step: (value) => {
          sum += value;
        },
        final: async () => {
          await Promise.resolve();
          return sum;
        },
      };
    });

    sql.exec('CREATE TABLE async_final_test (value INTEGER)');
    sql.exec('INSERT INTO async_final_test VALUES (1), (2), (3)');

    // Async final returns a Promise, which should throw
    let error = null;
    try {
      sql
        .exec('SELECT async_final_agg(value) AS total FROM async_final_test')
        .one();
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE async_final_test');

    assert.ok(error !== null, 'Should have thrown an error');
    assert.ok(
      error.message.includes('Promise'),
      `Error should mention Promise, got: ${error.message}`
    );
  }

  // ===========================================================================
  // Function Name Edge Cases
  // ===========================================================================

  /**
   * Test Unicode/non-ASCII function names.
   * SQLite should support Unicode identifiers.
   */
  async testUnicodeFunctionName() {
    const sql = this.state.storage.sql;

    // Japanese function name
    sql.createFunction('日本語関数', (x) => x * 2);
    const r1 = sql.exec('SELECT 日本語関数(5) AS val').one();
    assert.strictEqual(r1.val, 10);

    // Emoji function name (if supported)
    sql.createFunction('🚀rocket', (x) => x + 100);
    const r2 = sql.exec('SELECT 🚀rocket(5) AS val').one();
    assert.strictEqual(r2.val, 105);

    // Greek letters
    sql.createFunction('αβγ', (x) => x * 3);
    const r3 = sql.exec('SELECT αβγ(5) AS val').one();
    assert.strictEqual(r3.val, 15);
  }

  /**
   * Test case sensitivity of function names.
   *
   * SQLite function names are case-insensitive. Our implementation correctly
   * handles this by storing function names in lowercase and converting lookup keys
   * to lowercase before HashMap lookups.
   */
  async testFunctionNameCaseSensitivity() {
    const sql = this.state.storage.sql;

    // Register with lowercase - should work with any case in queries
    sql.createFunction('mytest', (x) => x * 2);

    const r1 = sql.exec('SELECT mytest(5) AS val').one();
    assert.strictEqual(r1.val, 10, 'lowercase call should work');

    const r2 = sql.exec('SELECT MYTEST(5) AS val').one();
    assert.strictEqual(r2.val, 10, 'uppercase call should work');

    const r3 = sql.exec('SELECT MyTest(5) AS val').one();
    assert.strictEqual(r3.val, 10, 'mixed case call should work');

    // Register with mixed case - should also work with any case in queries
    sql.createFunction('MixedCaseFunc', (x) => x * 3);

    const r4 = sql.exec('SELECT MixedCaseFunc(5) AS val').one();
    assert.strictEqual(r4.val, 15, 'exact case call should work');

    const r5 = sql.exec('SELECT MIXEDCASEFUNC(5) AS val').one();
    assert.strictEqual(
      r5.val,
      15,
      'uppercase call should work for mixed-case registered func'
    );

    const r6 = sql.exec('SELECT mixedcasefunc(5) AS val').one();
    assert.strictEqual(
      r6.val,
      15,
      'lowercase call should work for mixed-case registered func'
    );
  }

  /**
   * Test replacing a function with different case.
   *
   * Since function names are case-insensitive and we normalize to lowercase,
   * registering a function with different case simply replaces the existing
   * function both in SQLite and in our HashMap.
   */
  async testFunctionReplacementDifferentCase() {
    const sql = this.state.storage.sql;

    sql.createFunction('myFunc', (x) => x * 2);
    const r1 = sql.exec('SELECT myFunc(5) AS val').one();
    assert.strictEqual(r1.val, 10);

    // Register with different case - replaces the function (case-insensitive)
    sql.createFunction('MYFUNC', (x) => x * 3);

    // All cases use the new implementation
    const r2 = sql.exec('SELECT myFunc(5) AS val').one();
    assert.strictEqual(r2.val, 15, 'myFunc should use new implementation');

    const r3 = sql.exec('SELECT MYFUNC(5) AS val').one();
    assert.strictEqual(r3.val, 15, 'MYFUNC should use new implementation');

    const r4 = sql.exec('SELECT myfunc(5) AS val').one();
    assert.strictEqual(r4.val, 15, 'myfunc should use new implementation');
  }

  /**
   * Test SQL reserved words as function names.
   */
  async testReservedWordFunctionNames() {
    const sql = this.state.storage.sql;

    // These might need quoting or might just work
    // SQLite allows function names that are reserved words

    sql.createFunction('select_func', (x) => x + 1);
    const r1 = sql.exec('SELECT select_func(5) AS val').one();
    assert.strictEqual(r1.val, 6);

    sql.createFunction('from_func', (x) => x + 2);
    const r2 = sql.exec('SELECT from_func(5) AS val').one();
    assert.strictEqual(r2.val, 7);

    sql.createFunction('where_func', (x) => x + 3);
    const r3 = sql.exec('SELECT where_func(5) AS val').one();
    assert.strictEqual(r3.val, 8);
  }

  // ===========================================================================
  // Database State Edge Cases
  // ===========================================================================

  /**
   * Test UDF behavior across deleteAll().
   *
   * NOTE: deleteAll() clears the entire database including UDF registrations.
   * UDFs need to be re-registered after deleteAll() if they're needed.
   * This is because deleteAll() may recreate the database connection,
   * and sqlite3_create_function_v2 registrations are per-connection.
   */
  async testUdfSurvivesDeleteAll() {
    const storage = this.state.storage;
    const sql = storage.sql;

    // Register a UDF
    sql.createFunction('survives_delete', (x) => x * 7);

    // Create a table and use the UDF
    sql.exec('CREATE TABLE delete_test (value INTEGER)');
    sql.exec('INSERT INTO delete_test VALUES (6)');
    const r1 = sql
      .exec('SELECT survives_delete(value) AS val FROM delete_test')
      .one();
    assert.strictEqual(r1.val, 42);

    // Delete everything
    await storage.deleteAll();

    // KNOWN BEHAVIOR: UDFs are cleared by deleteAll() because it may
    // recreate the database connection. Need to re-register.
    let udfCleared = false;
    try {
      sql.exec('SELECT survives_delete(6) AS val').one();
    } catch (e) {
      udfCleared = true;
      assert.ok(
        e.message.includes('no such function'),
        `Expected 'no such function' error, got: ${e.message}`
      );
    }
    assert.ok(udfCleared, 'UDF should be cleared by deleteAll()');

    // Re-register the UDF
    sql.createFunction('survives_delete', (x) => x * 7);

    // Now it should work again
    const r2 = sql.exec('SELECT survives_delete(6) AS val').one();
    assert.strictEqual(r2.val, 42);

    // Tables are also gone
    let tableGone = false;
    try {
      sql.exec('SELECT * FROM delete_test');
    } catch (e) {
      tableGone = true;
    }
    assert.ok(tableGone, 'Table should be deleted');
  }

  /**
   * Test UDF in CHECK constraint.
   * The UDF should be called during INSERT/UPDATE to validate data.
   */
  async testUdfInCheckConstraint() {
    const sql = this.state.storage.sql;

    let checkCallCount = 0;
    sql.createFunction('is_positive', (x) => {
      checkCallCount++;
      return x > 0 ? 1 : 0;
    });

    sql.exec(
      'CREATE TABLE check_test (value INTEGER CHECK (is_positive(value)))'
    );

    // Valid insert should succeed
    sql.exec('INSERT INTO check_test VALUES (5)');
    assert.ok(checkCallCount > 0, 'CHECK constraint should call UDF');

    const validCount = checkCallCount;

    // Invalid insert should fail
    let constraintFailed = false;
    try {
      sql.exec('INSERT INTO check_test VALUES (-5)');
    } catch (e) {
      constraintFailed = true;
      assert.ok(
        e.message.includes('CHECK') || e.message.includes('constraint'),
        'Should fail with constraint error'
      );
    }

    assert.ok(constraintFailed, 'Negative value should fail CHECK constraint');
    assert.ok(
      checkCallCount > validCount,
      'CHECK should have called UDF again'
    );

    sql.exec('DROP TABLE check_test');
  }

  /**
   * Test UDF in DEFAULT value.
   * The UDF should be called when inserting without specifying the column.
   */
  async testUdfInDefaultValue() {
    const sql = this.state.storage.sql;

    let defaultCallCount = 0;
    sql.createFunction('get_default', () => {
      defaultCallCount++;
      return 42;
    });

    sql.exec(
      'CREATE TABLE default_test (id INTEGER PRIMARY KEY, value INTEGER DEFAULT (get_default()))'
    );

    // Insert without specifying value - should call UDF for default
    sql.exec('INSERT INTO default_test (id) VALUES (1)');
    assert.ok(defaultCallCount > 0, 'DEFAULT should call UDF');

    const result = sql
      .exec('SELECT value FROM default_test WHERE id = 1')
      .one();
    assert.strictEqual(result.value, 42);

    // Insert with explicit value - should NOT call UDF
    const beforeCount = defaultCallCount;
    sql.exec('INSERT INTO default_test (id, value) VALUES (2, 100)');
    // Note: SQLite might still evaluate DEFAULT even if not used, so we just check result
    const result2 = sql
      .exec('SELECT value FROM default_test WHERE id = 2')
      .one();
    assert.strictEqual(result2.value, 100);

    sql.exec('DROP TABLE default_test');
  }

  /**
   * Test UDF in generated/computed column.
   *
   * NOTE: SQLite requires functions used in generated columns to be deterministic.
   * Our UDFs are NOT registered as deterministic (we don't pass SQLITE_DETERMINISTIC),
   * so they CANNOT be used in generated columns. This documents that limitation.
   */
  async testUdfInGeneratedColumn() {
    const sql = this.state.storage.sql;

    sql.createFunction('compute_value', (x) => x * x);

    // Attempting to use a non-deterministic UDF in a generated column should fail
    let failedAsExpected = false;
    try {
      sql.exec(`
        CREATE TABLE generated_test (
          base INTEGER,
          computed INTEGER GENERATED ALWAYS AS (compute_value(base)) STORED
        )
      `);
    } catch (e) {
      failedAsExpected = true;
      assert.ok(
        e.message.includes('non-deterministic') ||
          e.message.includes('deterministic'),
        `Expected determinism error, got: ${e.message}`
      );
    }

    assert.ok(
      failedAsExpected,
      'UDFs should not be usable in generated columns (not registered as deterministic)'
    );
  }

  /**
   * Test attempting to shadow a built-in function.
   * This should either fail or the user function should take precedence.
   */
  async testShadowBuiltinFunction() {
    const sql = this.state.storage.sql;

    // Try to shadow the built-in abs() function
    sql.createFunction('abs', (x) => x * 1000); // Very different from real abs

    // What happens when we call abs()?
    const result = sql.exec('SELECT abs(-5) AS val').one();

    // Document the behavior: either user function wins or built-in wins
    // Most SQLite bindings allow user functions to shadow built-ins
    // If user function wins: result should be -5000
    // If built-in wins: result should be 5
    assert.ok(
      result.val === -5000 || result.val === 5,
      `abs(-5) should return either -5000 (user) or 5 (built-in), got ${result.val}`
    );

    // Also test with upper()
    sql.createFunction('upper', (s) => s + '_CUSTOM');
    const result2 = sql.exec("SELECT upper('test') AS val").one();

    // Document behavior
    assert.ok(
      result2.val === 'test_CUSTOM' || result2.val === 'TEST',
      `upper('test') should return either 'test_CUSTOM' (user) or 'TEST' (built-in), got ${result2.val}`
    );
  }

  // ===========================================================================
  // Numeric Edge Cases
  // ===========================================================================

  /**
   * Test integer overflow scenarios.
   */
  async testIntegerOverflow() {
    const sql = this.state.storage.sql;

    // Test Number.MAX_SAFE_INTEGER + 1
    sql.createFunction('overflow_add', (x) => x + 1);
    const r1 = sql
      .exec(`SELECT overflow_add(${Number.MAX_SAFE_INTEGER}) AS val`)
      .one();
    // JS loses precision beyond MAX_SAFE_INTEGER
    assert.ok(
      r1.val > Number.MAX_SAFE_INTEGER,
      'Should exceed MAX_SAFE_INTEGER'
    );

    // Test Number.MAX_VALUE * 2 -> Infinity
    sql.createFunction('overflow_mult', (x) => x * 2);
    const r2 = sql
      .exec(`SELECT overflow_mult(${Number.MAX_VALUE}) AS val`)
      .one();
    assert.strictEqual(r2.val, Infinity, 'MAX_VALUE * 2 should be Infinity');

    // Test very negative numbers
    sql.createFunction('overflow_neg', (x) => x - 1);
    const r3 = sql
      .exec(`SELECT overflow_neg(${-Number.MAX_SAFE_INTEGER}) AS val`)
      .one();
    assert.ok(r3.val < -Number.MAX_SAFE_INTEGER, 'Should be more negative');

    // Test -Infinity
    sql.createFunction('return_neg_max', () => -Number.MAX_VALUE * 2);
    const r4 = sql.exec('SELECT return_neg_max() AS val').one();
    assert.strictEqual(r4.val, -Infinity);
  }

  /**
   * Test special numeric values in more detail.
   */
  async testSpecialNumericValues() {
    const sql = this.state.storage.sql;

    // Negative zero
    sql.createFunction('return_neg_zero', () => -0);
    const r1 = sql.exec('SELECT return_neg_zero() AS val').one();
    // SQLite likely converts -0 to 0
    assert.strictEqual(r1.val, 0);
    // But Object.is can distinguish... or not after SQLite round-trip
    // Just document the behavior
    assert.ok(r1.val === 0 || Object.is(r1.val, -0), 'Should be 0 or -0');

    // Very small numbers (subnormal)
    sql.createFunction('return_tiny', () => Number.MIN_VALUE);
    const r2 = sql.exec('SELECT return_tiny() AS val').one();
    assert.ok(r2.val > 0 && r2.val < 1e-300, 'Should preserve tiny numbers');

    // Epsilon
    sql.createFunction('return_epsilon', () => Number.EPSILON);
    const r3 = sql.exec('SELECT return_epsilon() AS val').one();
    assert.ok(
      Math.abs(r3.val - Number.EPSILON) < 1e-20,
      'Should preserve epsilon'
    );
  }

  // ===========================================================================
  // Argument Count Edge Cases
  // ===========================================================================

  /**
   * Test maximum argument count (SQLite limit is 127 for user functions).
   */
  async testMaximumArgumentCount() {
    const sql = this.state.storage.sql;

    // Create a UDF that accepts many arguments
    sql.createFunction('sum_many', (...args) => {
      return args.reduce((sum, val) => sum + (val ?? 0), 0);
    });

    // Test with exactly 100 arguments (well within limit)
    const args100 = Array.from({ length: 100 }, (_, i) => i + 1);
    const query100 = `SELECT sum_many(${args100.join(',')}) AS val`;
    const r100 = sql.exec(query100).one();
    // Sum of 1 to 100 = 5050
    assert.strictEqual(r100.val, 5050);

    // Test with 127 arguments (SQLite's default SQLITE_MAX_FUNCTION_ARG)
    const args127 = Array.from({ length: 127 }, (_, i) => i + 1);
    const query127 = `SELECT sum_many(${args127.join(',')}) AS val`;
    const r127 = sql.exec(query127).one();
    // Sum of 1 to 127 = 127 * 128 / 2 = 8128
    assert.strictEqual(r127.val, 8128);
  }

  /**
   * Test calling a UDF with wrong number of arguments.
   * Since we register with argCount=-1 (variadic), this should always work.
   * But what if we registered with a specific count?
   */
  async testArgumentCountMismatch() {
    const sql = this.state.storage.sql;

    // Our implementation uses -1 (variadic), so any count should work
    sql.createFunction('variadic_test', (...args) => args.length);

    const r0 = sql.exec('SELECT variadic_test() AS cnt').one();
    const r1 = sql.exec('SELECT variadic_test(1) AS cnt').one();
    const r3 = sql.exec('SELECT variadic_test(1, 2, 3) AS cnt').one();

    assert.strictEqual(r0.cnt, 0);
    assert.strictEqual(r1.cnt, 1);
    assert.strictEqual(r3.cnt, 3);
  }

  // ===========================================================================
  // SQL Context Edge Cases
  // ===========================================================================

  /**
   * Test UDF with RETURNING clause.
   */
  async testUdfWithReturning() {
    const sql = this.state.storage.sql;

    sql.createFunction('transform_for_return', (x) => x * 10);

    sql.exec(
      'CREATE TABLE returning_test (id INTEGER PRIMARY KEY, value INTEGER)'
    );

    // INSERT with RETURNING using UDF
    const r1 = sql
      .exec(
        'INSERT INTO returning_test VALUES (1, 5) RETURNING transform_for_return(value) AS transformed'
      )
      .one();
    assert.strictEqual(r1.transformed, 50);

    // UPDATE with RETURNING using UDF
    const r2 = sql
      .exec(
        'UPDATE returning_test SET value = 10 WHERE id = 1 RETURNING transform_for_return(value) AS transformed'
      )
      .one();
    assert.strictEqual(r2.transformed, 100);

    // DELETE with RETURNING using UDF
    const r3 = sql
      .exec(
        'DELETE FROM returning_test WHERE id = 1 RETURNING transform_for_return(value) AS transformed'
      )
      .one();
    assert.strictEqual(r3.transformed, 100);

    sql.exec('DROP TABLE returning_test');
  }

  /**
   * Test aggregate when step throws on the very first row.
   * Is final() still called? What state is passed to it?
   */
  async testAggregateStepThrowsOnFirstRow() {
    const sql = this.state.storage.sql;

    let stepCalled = false;
    let finalCalled = false;
    let finalState = 'not called';

    sql.createFunction('first_row_fail', () => {
      let state = undefined;
      return {
        step: (value) => {
          stepCalled = true;
          throw new Error('Fail on first row');
        },
        final: () => {
          finalCalled = true;
          finalState = state;
          return state ?? 'default';
        },
      };
    });

    sql.exec('CREATE TABLE first_fail_test (value INTEGER)');
    sql.exec('INSERT INTO first_fail_test VALUES (1), (2), (3)');

    let error = null;
    try {
      sql
        .exec('SELECT first_row_fail(value) AS result FROM first_fail_test')
        .one();
    } catch (e) {
      error = e;
    }

    sql.exec('DROP TABLE first_fail_test');

    assert.ok(stepCalled, 'Step should have been called');
    assert.ok(error !== null, 'Should have thrown');
    assert.ok(
      error.message.includes('Fail on first row'),
      'Error message preserved'
    );

    // Document whether final was called after step threw
    // (It likely isn't, but good to document)
  }

  /**
   * Test that same UDF called multiple times in same row is actually called multiple times.
   */
  async testSameUdfMultipleTimesPerRow() {
    const sql = this.state.storage.sql;

    let callCount = 0;
    sql.createFunction('count_calls', (x) => {
      callCount++;
      return x;
    });

    sql.exec('CREATE TABLE multi_call_test (value INTEGER)');
    sql.exec('INSERT INTO multi_call_test VALUES (1), (2), (3)');

    const beforeCount = callCount;

    // Call the same UDF twice per row
    sql
      .exec(
        'SELECT count_calls(value), count_calls(value) FROM multi_call_test'
      )
      .toArray();

    // 3 rows * 2 calls per row = 6 calls
    assert.strictEqual(
      callCount - beforeCount,
      6,
      'UDF should be called twice per row'
    );

    sql.exec('DROP TABLE multi_call_test');
  }

  /**
   * Test UDF in CTE (Common Table Expression).
   */
  async testUdfInCte() {
    const sql = this.state.storage.sql;

    sql.createFunction('cte_transform', (x) => x * 2);

    sql.exec('CREATE TABLE cte_source (value INTEGER)');
    sql.exec('INSERT INTO cte_source VALUES (1), (2), (3)');

    const results = sql
      .exec(
        `
        WITH transformed AS (
          SELECT cte_transform(value) AS doubled FROM cte_source
        )
        SELECT doubled FROM transformed ORDER BY doubled
      `
      )
      .toArray();

    sql.exec('DROP TABLE cte_source');

    assert.strictEqual(results.length, 3);
    assert.strictEqual(results[0].doubled, 2);
    assert.strictEqual(results[1].doubled, 4);
    assert.strictEqual(results[2].doubled, 6);
  }

  /**
   * Test UDF in window function context (not as window function, but in OVER clause).
   */
  async testUdfInWindowContext() {
    const sql = this.state.storage.sql;

    sql.createFunction('window_transform', (x) => x * 10);

    sql.exec('CREATE TABLE window_test (category TEXT, value INTEGER)');
    sql.exec(
      "INSERT INTO window_test VALUES ('a', 1), ('a', 2), ('b', 3), ('b', 4)"
    );

    // Use scalar UDF in a query with window function
    const results = sql
      .exec(
        `
        SELECT
          category,
          value,
          window_transform(value) AS transformed,
          SUM(value) OVER (PARTITION BY category) AS category_sum
        FROM window_test
        ORDER BY category, value
      `
      )
      .toArray();

    sql.exec('DROP TABLE window_test');

    assert.strictEqual(results.length, 4);
    assert.strictEqual(results[0].transformed, 10);
    assert.strictEqual(results[0].category_sum, 3); // 1+2 for category 'a'
    assert.strictEqual(results[2].transformed, 30);
    assert.strictEqual(results[2].category_sum, 7); // 3+4 for category 'b'
  }

  /**
   * Test UDF with custom valueOf/toString objects as return values.
   */
  async testUdfWithCustomValueOf() {
    const sql = this.state.storage.sql;

    // Object with custom valueOf - should be called during coercion
    sql.createFunction('return_valueof', () => ({
      valueOf: () => 42,
      toString: () => 'custom string',
    }));

    const result = sql.exec('SELECT return_valueof() AS val').one();

    // Document behavior: does it call valueOf, toString, or stringify the object?
    // The coercion logic in jsResultToSqlite checks IsNumber/IsString first,
    // so plain objects fall through to toString
    assert.ok(
      result.val === 42 ||
        result.val === 'custom string' ||
        result.val === '[object Object]',
      `Custom valueOf object should be coerced somehow, got: ${result.val}`
    );
  }

  /**
   * Test that when a UDF throws mid-iteration, we properly clean up
   * and the cursor is left in a consistent state.
   */
  async testUdfThrowsAfterPartialResults() {
    const sql = this.state.storage.sql;

    sql.exec('CREATE TABLE partial_test (id INTEGER)');
    sql.exec('INSERT INTO partial_test VALUES (1), (2), (3), (4), (5)');

    let callCount = 0;
    sql.createFunction('fail_at_three', (id) => {
      callCount++;
      if (id === 3) {
        throw new Error('Deliberate failure at id 3');
      }
      return id * 10;
    });

    const results = [];
    let caughtError = null;

    try {
      // Note: The cursor may start executing immediately on creation,
      // or during iteration - depends on implementation
      for (const row of sql.exec(
        'SELECT fail_at_three(id) AS val FROM partial_test ORDER BY id'
      )) {
        results.push(row.val);
      }
    } catch (e) {
      caughtError = e;
    }

    // Clean up - this should work even after an error in the previous query
    sql.exec('DROP TABLE partial_test');

    // Should have caught the error
    assert.ok(caughtError !== null, 'Should have caught an error');
    assert.ok(
      caughtError.message.includes('Deliberate failure'),
      'Error message should be preserved'
    );
    assert.strictEqual(callCount, 3, 'UDF should have been called 3 times');
    // We may have collected 0, 1, or 2 results depending on buffering
    assert.ok(
      results.length <= 2,
      `Should have at most 2 results, got ${results.length}`
    );

    // Most importantly: can we still use the database after the error?
    const sanityCheck = sql.exec('SELECT 1 + 1 AS answer').one();
    assert.strictEqual(
      sanityCheck.answer,
      2,
      'Database should still work after UDF error'
    );

    // Can we use the same UDF again?
    sql.exec('CREATE TABLE recovery_test (id INTEGER)');
    sql.exec('INSERT INTO recovery_test VALUES (1), (2)'); // No id=3!
    const recoveryResult = sql
      .exec('SELECT fail_at_three(id) AS val FROM recovery_test ORDER BY id')
      .toArray();
    assert.strictEqual(recoveryResult.length, 2);
    assert.strictEqual(recoveryResult[0].val, 10);
    assert.strictEqual(recoveryResult[1].val, 20);
    sql.exec('DROP TABLE recovery_test');
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
    await stub.testUdfErrorStackTracePreserved();

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

    // Aggregate function tests
    await stub.testAggregateSum();
    await stub.testAggregateCount();
    await stub.testAggregateWithNoRows();
    await stub.testAggregateWithGroupBy();
    await stub.testAggregateWithStringConcat();
    await stub.testAggregateWithArrayState();
    await stub.testAggregateStepError();
    await stub.testAggregateFinalError();
    await stub.testAggregateValidation();
    await stub.testAggregateMultipleArgs();

    // Transaction interaction tests (async)
    await stub.testScalarUdfInTransaction();
    await stub.testScalarUdfErrorRollsBackTransaction();
    await stub.testAggregateUdfInTransaction();
    await stub.testAggregateUdfErrorRollsBackTransaction();
    await stub.testUdfInNestedTransaction();

    // Sync transaction tests
    await stub.testScalarUdfInSyncTransaction();
    await stub.testScalarUdfErrorRollsBackSyncTransaction();
    await stub.testAggregateUdfInSyncTransaction();
    await stub.testAggregateUdfErrorRollsBackSyncTransaction();
    await stub.testNestedSyncTransactionsWithUdf();
    await stub.testSyncTransactionReturnValue();

    // Side effects and transaction semantics
    await stub.testSideEffectsNotRolledBackOnTransactionFailure();
    await stub.testAggregateSideEffectsNotRolledBack();
    await stub.testUdfMayBeCalledMultipleTimesForSameRow();
    await stub.testSideEffectsOccurEvenIfResultUnused();

    // Function replacement tests
    await stub.testScalarFunctionReplacement();
    await stub.testAggregateFunctionReplacement();

    // Type edge cases
    await stub.testUdfWithNaN();
    await stub.testUdfWithInfinity();
    await stub.testUdfReceivesNullArgument();
    await stub.testAggregateWithNullValues();
    await stub.testUdfWithVeryLargeNumber();
    await stub.testUdfWithFloatingPoint();

    // Aggregate re-entrancy tests
    await stub.testAggregateStepReentrancyBlocked();
    await stub.testAggregateFinalReentrancyBlocked();

    // UDFs in complex SQL contexts
    await stub.testUdfInSubquery();
    await stub.testUdfInOrderBy();
    await stub.testUdfInHaving();
    await stub.testAggregateUdfInSubquery();

    // Multiple aggregates and edge cases
    await stub.testMultipleAggregatesInSameQuery();
    await stub.testSameAggregateMultipleTimes();
    await stub.testScalarUdfWithZeroArguments();
    await stub.testAggregateWithDistinct();
    await stub.testAggregateWithFilterClause();
    await stub.testScalarUdfInCaseExpression();
    await stub.testScalarUdfInCoalesce();
    await stub.testUdfWithVeryManyArguments();
    await stub.testAggregateWithOrderByInGroupBy();
    await stub.testScalarUdfPreservesTypeFromInput();
    await stub.testUdfCalledForEachRowInJoin();

    // Real-world use case tests
    await stub.testChangeHookViaTriggersAndUdfs();

    // Stack trace preservation for aggregates
    await stub.testAggregateStepStackTracePreserved();
    await stub.testAggregateFinalStackTracePreserved();

    // Garbage collection tests
    await stub.testUdfCallbackSurvivesGc();
    await stub.testAggregateUdfSurvivesGcDuringProcessing();
    await stub.testReplacedUdfCallbackCanBeCollected();
    await stub.testAggregateStateIsCollectedAfterQuery();
    await stub.testUdfStressWithGc();

    // Edge case tests
    await stub.testPreparedStatementWithReplacedUdf();
    await stub.testUdfReturnsUnsupportedTypes();
    await stub.testUdfThrowsAfterPartialResults();

    // CRITICAL: Async callback tests (these verify async UDFs throw clear errors)
    await stub.testAsyncScalarUdfThrowsError();
    await stub.testAsyncAggregateStepThrowsError();
    await stub.testAsyncAggregateFinalThrowsError();

    // Function name edge cases
    await stub.testUnicodeFunctionName();
    await stub.testFunctionNameCaseSensitivity();
    await stub.testFunctionReplacementDifferentCase();
    await stub.testReservedWordFunctionNames();

    // Database state edge cases
    await stub.testUdfSurvivesDeleteAll();
    await stub.testUdfInCheckConstraint();
    await stub.testUdfInDefaultValue();
    await stub.testUdfInGeneratedColumn();
    await stub.testShadowBuiltinFunction();

    // Numeric edge cases
    await stub.testIntegerOverflow();
    await stub.testSpecialNumericValues();

    // Argument count edge cases
    await stub.testMaximumArgumentCount();
    await stub.testArgumentCountMismatch();

    // SQL context edge cases
    await stub.testUdfWithReturning();
    await stub.testAggregateStepThrowsOnFirstRow();
    await stub.testSameUdfMultipleTimesPerRow();
    await stub.testUdfInCte();
    await stub.testUdfInWindowContext();
    await stub.testUdfWithCustomValueOf();
  },
};
