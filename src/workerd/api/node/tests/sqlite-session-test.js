// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

import * as sqliteMod from 'node:sqlite';
import sqliteDefault from 'node:sqlite';

export const sqliteSessionTest = {
  test() {
    assert.strictEqual(typeof sqliteMod.Session, 'function');
    assert.strictEqual(typeof sqliteDefault.Session, 'function');
    assert.throws(() => new sqliteMod.Session(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};
