import { DatabaseSync, StatementSync, backup } from 'node:sqlite';

export { DatabaseSync, StatementSync, backup };

export const SQLITE_CHANGESET_OMIT: number;
export const SQLITE_CHANGESET_REPLACE: number;
export const SQLITE_CHANGESET_ABORT: number;
export const SQLITE_CHANGESET_DATA: number;
export const SQLITE_CHANGESET_NOTFOUND: number;
export const SQLITE_CHANGESET_CONFLICT: number;
export const SQLITE_CHANGESET_CONSTRAINT: number;
export const SQLITE_CHANGESET_FOREIGN_KEY: number;
