// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { DurableObject } from 'cloudflare:workers';
import assert from 'node:assert';

// A read-only query that burns CPU for far longer than any limit used here.
const LONG_READ =
  'WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 1000000000) ' +
  'SELECT count(*) FROM c';

// A write that burns CPU for far longer than any limit used here.
const LONG_WRITE =
  'WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 1000000000) ' +
  'INSERT INTO things(value) SELECT x FROM c';

const LIMIT_MS = 20;

function assertTimeoutError(err) {
  assert.ok(err instanceof DOMException, `expected DOMException, got ${err}`);
  assert.strictEqual(err.name, 'TimeoutError');
  assert.match(err.message, /SQL query exceeded its CPU time limit of 20ms/);
  return true;
}

export class CpuTimeLimitObject extends DurableObject {
  constructor(state, env) {
    super(state, env);
    this.sql = state.storage.sql;
    this.storage = state.storage;
  }

  testReads() {
    const { sql } = this;

    // The options form works like the string form when no limit is given.
    assert.deepEqual([...sql.exec({ sql: 'SELECT 1 AS n' })], [{ n: 1 }]);
    assert.deepEqual([...sql.exec({ sql: 'SELECT ? AS n' }, 7)], [{ n: 7 }]);

    // A query that stays within its limit is unaffected.
    assert.deepEqual(
      [...sql.exec({ sql: 'SELECT ? AS n', cpuTimeLimitMs: 10000 }, 8)],
      [{ n: 8 }]
    );

    // A long read is interrupted.
    assert.throws(
      () => sql.exec({ sql: LONG_READ, cpuTimeLimitMs: LIMIT_MS }),
      assertTimeoutError
    );

    // ... and the object is still perfectly usable afterwards.
    assert.deepEqual([...sql.exec('SELECT 2 AS n')], [{ n: 2 }]);

    // A cursor that streams many cheap rows is bounded as a whole.
    const cursor = sql.exec({
      sql: 'WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT x FROM c',
      cpuTimeLimitMs: LIMIT_MS,
    });
    let rows = 0;
    assert.throws(() => {
      for (const _ of cursor) rows++;
    }, assertTimeoutError);
    assert.ok(rows > 0);

    // Inside transactionSync(), an interrupted read is an ordinary exception: the transaction
    // rolls back and the object carries on.
    sql.exec('CREATE TABLE things(value INTEGER)');
    sql.exec('INSERT INTO things VALUES (1)');
    assert.throws(
      () =>
        this.storage.transactionSync(() => {
          sql.exec('INSERT INTO things VALUES (2)');
          sql.exec({ sql: LONG_READ, cpuTimeLimitMs: LIMIT_MS });
        }),
      assertTimeoutError
    );
    assert.deepEqual([...sql.exec('SELECT value FROM things')], [{ value: 1 }]);
    sql.exec('INSERT INTO things VALUES (3)');
    assert.deepEqual(
      [...sql.exec('SELECT value FROM things ORDER BY value')],
      [{ value: 1 }, { value: 3 }]
    );

    // The limit must be a positive number.
    for (const bad of [0, -1, NaN]) {
      assert.throws(() => sql.exec({ sql: 'SELECT 1', cpuTimeLimitMs: bad }), {
        name: 'TypeError',
        message: /cpuTimeLimitMs must be a positive number/,
      });
    }
  }

  countThings() {
    return this.sql.exec('SELECT count(*) AS n FROM things').one().n;
  }

  testWrite() {
    // The statement throws a TimeoutError like a read does...
    assert.throws(
      () => this.sql.exec({ sql: LONG_WRITE, cpuTimeLimitMs: LIMIT_MS }),
      assertTimeoutError
    );
    // ... but SQLite has rolled back the whole transaction, so the object is now broken and this
    // call's completion is held back by the output gate. The caller sees the breakage.
    return 'unreachable from the caller';
  }
}

export const reads = {
  async test(ctrl, env) {
    const stub = env.ns.get(env.ns.idFromName('reads'));
    await stub.testReads();
  },
};

export const writeResetsTheObject = {
  async test(ctrl, env) {
    const id = env.ns.idFromName('write');
    let stub = env.ns.get(id);
    await stub.testReads(); // creates `things` with two rows
    assert.strictEqual(await stub.countThings(), 2);

    // The interrupted write breaks the object's output gate; the failure names the cause.
    await assert.rejects(
      () => stub.testWrite(),
      (err) =>
        /SQL query exceeded its CPU time limit of 20ms/.test(err.message) &&
        err.durableObjectReset === true
    );

    // A fresh stub finds the object reset, with the interrupted write rolled back.
    stub = env.ns.get(id);
    assert.strictEqual(await stub.countThings(), 2);
  },
};

export const requiresExperimental = {
  async test(ctrl, env) {
    const stub = env.noExperimentalNs.get(env.noExperimentalNs.idFromName('x'));
    await stub.testOptionsRejected();
  },
};
