// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for the SQL_RESTRICT_RESERVED_NAMES autogate.
// This test only runs in the @all-autogates variant so it can assert the gated behavior.

import * as assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

export class DurableObjectExample extends DurableObject {
  async fetch(req) {
    const sql = this.ctx.storage.sql;

    // Case-insensitive _cf_ prefix blocking: mixed-case variants are rejected.
    for (const name of ['_CF_test', '_Cf_test', '_cF_test']) {
      assert.throws(
        () => sql.exec(`CREATE TABLE ${name} (name TEXT)`),
        /not authorized/,
        `Expected CREATE TABLE ${name} to be blocked`
      );
    }

    // Virtual tables with _cf_ prefix are blocked.
    assert.throws(
      () => sql.exec('CREATE VIRTUAL TABLE _cf_fts_test USING fts5(content)'),
      /not authorized/
    );
    assert.throws(
      () => sql.exec('CREATE VIRTUAL TABLE _CF_fts_test USING fts5(content)'),
      /not authorized/
    );

    // Non-_cf_ prefixed virtual tables still work.
    sql.exec('CREATE VIRTUAL TABLE my_fts USING fts5(content)');
    sql.exec('DROP TABLE my_fts');

    return Response.json({ ok: true });
  }
}

export default {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName('sql-restrict-names');
    let stub = env.ns.get(id);
    let response = await stub.fetch('http://x/test');
    let result = await response.json();
    assert.ok(result.ok);
  },
};
