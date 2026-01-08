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

    // Name at exactly 255 bytes should be accepted (will fail at "not yet implemented")
    const maxName = 'x'.repeat(255);
    assert.throws(() => sql.createFunction(maxName, () => 42), {
      name: 'Error',
      message: 'createFunction is not yet implemented',
    });
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
  },
};
