// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import sqlite from 'node:sqlite';
import { deepStrictEqual, notStrictEqual, throws } from 'node:assert';

export const testExports = {
  async test() {
    notStrictEqual(sqlite.DatabaseSync, undefined);
    notStrictEqual(sqlite.StatementSync, undefined);
    notStrictEqual(sqlite.backup, undefined);
    notStrictEqual(sqlite.constants, undefined);

    deepStrictEqual(sqlite.constants, {
      SQLITE_CHANGESET_OMIT: 0,
      SQLITE_CHANGESET_REPLACE: 1,
      SQLITE_CHANGESET_ABORT: 2,
      SQLITE_CHANGESET_DATA: 1,
      SQLITE_CHANGESET_NOTFOUND: 2,
      SQLITE_CHANGESET_CONFLICT: 3,
      SQLITE_CHANGESET_CONSTRAINT: 4,
      SQLITE_CHANGESET_FOREIGN_KEY: 5,
      // Authorization action codes
      SQLITE_OK: 0,
      SQLITE_DENY: 1,
      SQLITE_IGNORE: 2,
      SQLITE_CREATE_INDEX: 1,
      SQLITE_CREATE_TABLE: 2,
      SQLITE_CREATE_TEMP_INDEX: 3,
      SQLITE_CREATE_TEMP_TABLE: 4,
      SQLITE_CREATE_TEMP_TRIGGER: 5,
      SQLITE_CREATE_TEMP_VIEW: 6,
      SQLITE_CREATE_TRIGGER: 7,
      SQLITE_CREATE_VIEW: 8,
      SQLITE_DELETE: 9,
      SQLITE_DROP_INDEX: 10,
      SQLITE_DROP_TABLE: 11,
      SQLITE_DROP_TEMP_INDEX: 12,
      SQLITE_DROP_TEMP_TABLE: 13,
      SQLITE_DROP_TEMP_TRIGGER: 14,
      SQLITE_DROP_TEMP_VIEW: 15,
      SQLITE_DROP_TRIGGER: 16,
      SQLITE_DROP_VIEW: 17,
      SQLITE_INSERT: 18,
      SQLITE_PRAGMA: 19,
      SQLITE_READ: 20,
      SQLITE_SELECT: 21,
      SQLITE_TRANSACTION: 22,
      SQLITE_UPDATE: 23,
      SQLITE_ATTACH: 24,
      SQLITE_DETACH: 25,
      SQLITE_ALTER_TABLE: 26,
      SQLITE_REINDEX: 27,
      SQLITE_ANALYZE: 28,
      SQLITE_CREATE_VTABLE: 29,
      SQLITE_DROP_VTABLE: 30,
      SQLITE_FUNCTION: 31,
      SQLITE_SAVEPOINT: 32,
      SQLITE_COPY: 0,
      SQLITE_RECURSIVE: 33,
    });

    throws(() => sqlite.backup(), {
      message: /is not implemented/,
    });

    throws(() => new sqlite.DatabaseSync(), {
      message: /Illegal constructor/,
    });

    throws(() => new sqlite.StatementSync(), {
      message: /Illegal constructor/,
    });
  },
};
