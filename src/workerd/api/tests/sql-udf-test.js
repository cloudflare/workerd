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

  // Placeholder for future UDF tests - will be added as we implement the feature
  // async testCreateFunctionExists() {
  //   const sql = this.state.storage.sql;
  //   assert.strictEqual(typeof sql.createFunction, 'function');
  // }
}

// =============================================================================
// Test Exports
// =============================================================================

export default {
  async test(ctrl, env, ctx) {
    const id = env.ns.idFromName('udf-test');
    const stub = env.ns.get(id);
    await stub.testSanityCheck();
  },
};
