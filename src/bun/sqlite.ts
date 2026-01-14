// bun:sqlite - NOT AVAILABLE IN WORKERD
//
// SQLite is not supported in workerd. For database access, use an API:
// - Cloudflare D1: https://developers.cloudflare.com/d1/
// - External database via HTTP
// - KV for key-value storage

export class Database {
  constructor(_filename?: string) {
    throw new Error(
      'bun:sqlite is not available in workerd.\n\n' +
        'For database access, use:\n' +
        '  - Cloudflare D1 (recommended)\n' +
        '  - External database via HTTP API\n' +
        '  - KV bindings for key-value storage',
    )
  }
}

export class Statement {
  constructor() {
    throw new Error('bun:sqlite is not available in workerd')
  }
}

// Constants for API compatibility
export const SQLITE_VERSION = 'unavailable'
export const SQLITE_VERSION_NUMBER = 0
export const OPEN_READONLY = 1
export const OPEN_READWRITE = 2
export const OPEN_CREATE = 4
export const SQLITE_OK = 0
export const SQLITE_ERROR = 1

export default { Database, Statement, SQLITE_VERSION, SQLITE_VERSION_NUMBER }
