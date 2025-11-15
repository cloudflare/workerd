import { default as sqliteUtil } from 'node-internal:sqlite';
import type sqlite from 'node:sqlite';

const {
  DatabaseSync,
  StatementSync,
  SQLITE_CHANGESET_OMIT,
  SQLITE_CHANGESET_REPLACE,
  SQLITE_CHANGESET_ABORT,
  SQLITE_CHANGESET_DATA,
  SQLITE_CHANGESET_NOTFOUND,
  SQLITE_CHANGESET_CONFLICT,
  SQLITE_CHANGESET_CONSTRAINT,
  SQLITE_CHANGESET_FOREIGN_KEY,
} = sqliteUtil;

export const backup = sqliteUtil.backup.bind(sqliteUtil);

export const constants: typeof sqlite.constants = {
  SQLITE_CHANGESET_OMIT,
  SQLITE_CHANGESET_REPLACE,
  SQLITE_CHANGESET_ABORT,
  SQLITE_CHANGESET_DATA,
  SQLITE_CHANGESET_NOTFOUND,
  SQLITE_CHANGESET_CONFLICT,
  // @ts-expect-error TS2561 This is missing from node.js types
  SQLITE_CHANGESET_CONSTRAINT,
  SQLITE_CHANGESET_FOREIGN_KEY,
};

export { DatabaseSync, StatementSync };

export default {
  DatabaseSync,
  StatementSync,
  constants,
  backup,
} satisfies typeof sqlite;
