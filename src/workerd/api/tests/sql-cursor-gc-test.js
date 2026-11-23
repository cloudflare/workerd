// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, strictEqual } from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

const QUERY = 'SELECT 1 AS x UNION ALL SELECT 2';

function iterate(cursor, kind) {
  return kind === 'raw' ? cursor.raw() : cursor[Symbol.iterator]();
}

async function collectGarbage() {
  for (let i = 0; i < 4; i++) {
    globalThis.gc();
    await scheduler.wait(1);
  }
}

export class DurableObjectExample extends DurableObject {
  // Returns whether the cursor is collected once nothing outside it references it. With `cycle`,
  // the cursor and its iterator reach each other only through JS.
  async collected(kind, cycle) {
    const weak = new WeakRef(
      (() => {
        const cursor = this.ctx.storage.sql.exec(QUERY);
        const it = iterate(cursor, kind);
        if (cycle) cursor.iterator = it;
        return cursor;
      })()
    );
    await collectGarbage();
    return weak.deref() === undefined;
  }

  // An iterator keeps alive a cursor that JS reaches only through the iterator.
  async iterateAfterGc(kind) {
    const it = iterate(this.ctx.storage.sql.exec(QUERY), kind);
    await collectGarbage();
    return [...it];
  }
}

// The cycle is collected only with the gc-complete-tracing autogate, which
// sql-cursor-gc-gate-off-test disables.
async function check(env, kind, rows) {
  const s = env.ns.get(env.ns.idFromName(kind));
  strictEqual(await s.collected(kind, false), true);
  strictEqual(await s.collected(kind, true), env.gcCompleteTracing);
  deepStrictEqual(await s.iterateAfterGc(kind), rows);
}

export const rowIterator = {
  async test(_ctrl, env) {
    await check(env, 'rows', [{ x: 1 }, { x: 2 }]);
  },
};

export const rawIterator = {
  async test(_ctrl, env) {
    await check(env, 'raw', [[1], [2]]);
  },
};
