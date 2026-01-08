// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

// =============================================================================
// Test: Basic SQL functionality works (sanity check)
// =============================================================================

export class UdfTestDO extends DurableObject {
  constructor(state, env) {
    super(state, env);
    this.state = state;
  }

  async testSanityCheck() {
    // Verify basic SQL works before we start testing UDFs
    const sql = this.state.storage.sql;
    const result = sql.exec('SELECT 42 AS answer').one();
    assert.strictEqual(result.answer, 42);
  }

  async testCreateFunctionExists() {
    // Verify createFunction method exists on the sql object
    const sql = this.state.storage.sql;
    assert.strictEqual(typeof sql.createFunction, 'function');
  }

  async testCreateFunctionValidatesName() {
    const sql = this.state.storage.sql;

    // Empty name should throw TypeError
    assert.throws(() => sql.createFunction('', () => 42), {
      name: 'TypeError',
      message: 'Function name cannot be empty.',
    });

    // Name too long (> 255 bytes) should throw TypeError
    const longName = 'x'.repeat(256);
    assert.throws(() => sql.createFunction(longName, () => 42), {
      name: 'TypeError',
      message: 'Function name is too long (max 255 bytes).',
    });

    // Name at exactly 255 bytes should be accepted (no error)
    const maxName = 'x'.repeat(255);
    sql.createFunction(maxName, () => 42);
  }

  async testSimpleScalarUdf() {
    const sql = this.state.storage.sql;

    // Register a simple function that doubles its argument
    sql.createFunction('my_double', (x) => x * 2);

    // Test using the function in a query
    const result = sql.exec('SELECT my_double(21) AS answer').one();
    assert.strictEqual(result.answer, 42);
  }

  async testUdfWithMultipleArgs() {
    const sql = this.state.storage.sql;

    // Register a function that adds two numbers
    sql.createFunction('my_add', (a, b) => a + b);

    // Test using the function
    const result = sql.exec('SELECT my_add(10, 32) AS answer').one();
    assert.strictEqual(result.answer, 42);
  }

  async testUdfWithStringResult() {
    const sql = this.state.storage.sql;

    // Register a function that returns a greeting
    sql.createFunction('greet', (name) => `Hello, ${name}!`);

    // Test using the function
    const result = sql.exec("SELECT greet('World') AS greeting").one();
    assert.strictEqual(result.greeting, 'Hello, World!');
  }

  async testUdfReturnsNull() {
    const sql = this.state.storage.sql;

    // Register a function that returns null
    sql.createFunction('get_null', () => null);

    // Test using the function
    const result = sql.exec('SELECT get_null() AS val').one();
    assert.strictEqual(result.val, null);
  }

  async testUdfThrowsError() {
    const sql = this.state.storage.sql;

    // Register a function that throws an error
    sql.createFunction('throw_error', () => {
      throw new Error('UDF error message');
    });

    // Test that the error propagates when the function is called
    assert.throws(
      () => sql.exec('SELECT throw_error() AS val').one(),
      (err) => {
        // The error should contain our message
        return err.message.includes('UDF error message');
      }
    );
  }

  async testUdfThrowsTypeError() {
    const sql = this.state.storage.sql;

    // Register a function that throws a TypeError
    sql.createFunction('throw_type_error', () => {
      throw new TypeError('Invalid type in UDF');
    });

    // Test that the error propagates
    assert.throws(
      () => sql.exec('SELECT throw_type_error() AS val').one(),
      (err) => {
        return err.message.includes('Invalid type in UDF');
      }
    );
  }

  async testUdfReturnsUndefined() {
    const sql = this.state.storage.sql;

    // Register a function that returns undefined (no return statement)
    sql.createFunction('get_undefined', () => {});

    // Undefined should be treated as null
    const result = sql.exec('SELECT get_undefined() AS val').one();
    assert.strictEqual(result.val, null);
  }

  async testUdfWithBlobInput() {
    const sql = this.state.storage.sql;

    // Create a table with a blob column
    sql.exec('CREATE TABLE IF NOT EXISTS blob_test (data BLOB)');
    const testBlob = new Uint8Array([1, 2, 3, 4, 5]);
    sql.exec('INSERT INTO blob_test VALUES (?)', testBlob);

    // Register a function that computes length of a blob
    // Blobs come in as ArrayBuffer, so we need to handle both
    sql.createFunction('blob_len', (blob) => {
      if (blob instanceof Uint8Array) {
        return blob.length;
      } else if (blob instanceof ArrayBuffer) {
        return blob.byteLength;
      } else if (ArrayBuffer.isView(blob)) {
        return blob.byteLength;
      }
      return -1;
    });

    // Test using the function with blob data
    const result = sql
      .exec('SELECT blob_len(data) AS len FROM blob_test')
      .one();
    assert.strictEqual(result.len, 5);

    // Cleanup
    sql.exec('DROP TABLE blob_test');
  }

  async testUdfWithBlobOutput() {
    const sql = this.state.storage.sql;

    // Register a function that returns a blob
    sql.createFunction('make_blob', (size) => {
      const arr = new Uint8Array(size);
      for (let i = 0; i < size; i++) {
        arr[i] = i % 256;
      }
      return arr;
    });

    // Test using the function
    const result = sql.exec('SELECT make_blob(5) AS blob').one();
    assert.ok(
      result.blob instanceof Uint8Array || result.blob instanceof ArrayBuffer
    );
    const view = new Uint8Array(result.blob);
    assert.strictEqual(view.length, 5);
    assert.strictEqual(view[0], 0);
    assert.strictEqual(view[4], 4);
  }
}

// =============================================================================
// Test Exports
// =============================================================================

export default {
  async test(ctrl, env, ctx) {
    const id = env.ns.idFromName('udf-test');
    const stub = env.ns.get(id);
    await stub.testSanityCheck();
    await stub.testCreateFunctionExists();
    await stub.testCreateFunctionValidatesName();
    await stub.testSimpleScalarUdf();
    await stub.testUdfWithMultipleArgs();
    await stub.testUdfWithStringResult();
    await stub.testUdfReturnsNull();
    await stub.testUdfThrowsError();
    await stub.testUdfThrowsTypeError();
    await stub.testUdfReturnsUndefined();
    await stub.testUdfWithBlobInput();
    await stub.testUdfWithBlobOutput();
  },
};
