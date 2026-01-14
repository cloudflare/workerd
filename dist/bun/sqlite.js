// sqlite.ts
var Database = class {
  constructor(_filename) {
    throw new Error(
      "bun:sqlite is not available in workerd.\n\nFor database access, use:\n  - Cloudflare D1 (recommended)\n  - External database via HTTP API\n  - KV bindings for key-value storage"
    );
  }
};
var Statement = class {
  constructor() {
    throw new Error("bun:sqlite is not available in workerd");
  }
};
var SQLITE_VERSION = "unavailable";
var SQLITE_VERSION_NUMBER = 0;
var OPEN_READONLY = 1;
var OPEN_READWRITE = 2;
var OPEN_CREATE = 4;
var SQLITE_OK = 0;
var SQLITE_ERROR = 1;
var sqlite_default = { Database, Statement, SQLITE_VERSION, SQLITE_VERSION_NUMBER };
export {
  Database,
  OPEN_CREATE,
  OPEN_READONLY,
  OPEN_READWRITE,
  SQLITE_ERROR,
  SQLITE_OK,
  SQLITE_VERSION,
  SQLITE_VERSION_NUMBER,
  Statement,
  sqlite_default as default
};
