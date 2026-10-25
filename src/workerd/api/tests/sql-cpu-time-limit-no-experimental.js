// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { DurableObject } from 'cloudflare:workers';
import assert from 'node:assert';

// Lives in a worker without the `experimental` flag: the options form of exec() is rejected.
export class NoExperimentalObject extends DurableObject {
  testOptionsRejected() {
    const sql = this.ctx.storage.sql;
    assert.deepEqual([...sql.exec('SELECT 1 AS n')], [{ n: 1 }]);
    assert.throws(() => sql.exec({ sql: 'SELECT 1 AS n' }), {
      name: 'TypeError',
      message: /requires the 'experimental' compatibility flag/,
    });
  }
}
