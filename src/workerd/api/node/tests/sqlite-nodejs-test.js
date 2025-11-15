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
