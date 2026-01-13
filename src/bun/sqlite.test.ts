// Copyright (c) 2024 Jeju Network
// Tests for bun:sqlite compatibility layer
// Licensed under the Apache 2.0 license

import { afterAll, afterEach, beforeAll, beforeEach, describe, expect, test } from 'bun:test'
import {
  Database,
  OPEN_CREATE,
  OPEN_READONLY,
  OPEN_READWRITE,
  SQLITE_OK,
  SQLITE_VERSION,
  SQLITE_VERSION_NUMBER,
  Statement,
} from './sqlite'

describe('bun:sqlite', () => {
  let db: Database

  beforeEach(() => {
    db = new Database(':memory:')
  })

  afterEach(() => {
    db.close()
  })

  describe('Database', () => {
    test('creates in-memory database', () => {
      expect(db.open).toBe(true)
      expect(db.inMemory).toBe(true)
      expect(db.path).toBe(':memory:')
    })

    test('closes database', () => {
      db.close()
      expect(db.open).toBe(false)
    })

    test('exec creates table', () => {
      db.exec('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)')
      const rows = db.query('SELECT * FROM users')
      expect(rows).toEqual([])
    })

    test('exec with multiple statements', () => {
      db.exec(`
        CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);
        CREATE TABLE posts (id INTEGER PRIMARY KEY, title TEXT)
      `)

      // Both tables should exist
      const users = db.query('SELECT * FROM users')
      const posts = db.query('SELECT * FROM posts')
      expect(users).toEqual([])
      expect(posts).toEqual([])
    })

    test('run inserts data', () => {
      db.exec(
        'CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)',
      )

      const result = db.run('INSERT INTO users (name) VALUES (?)', 'Alice')
      expect(result.changes).toBe(1)
      expect(result.lastInsertRowid).toBe(1)
    })

    test('query returns rows', () => {
      db.exec(
        'CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)',
      )
      db.run('INSERT INTO users (name) VALUES (?)', 'Alice')
      db.run('INSERT INTO users (name) VALUES (?)', 'Bob')

      const rows = db.query<{ id: number; name: string }>('SELECT * FROM users')
      expect(rows.length).toBe(2)
      expect(rows[0].name).toBe('Alice')
      expect(rows[1].name).toBe('Bob')
    })

    test('query with WHERE clause', () => {
      db.exec(
        'CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)',
      )
      db.run('INSERT INTO users (name) VALUES (?)', 'Alice')
      db.run('INSERT INTO users (name) VALUES (?)', 'Bob')

      const rows = db.query<{ id: number; name: string }>(
        'SELECT * FROM users WHERE name = ?',
        'Alice',
      )
      expect(rows.length).toBe(1)
      expect(rows[0].name).toBe('Alice')
    })

    test('query with ORDER BY', () => {
      db.exec(
        'CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)',
      )
      db.run('INSERT INTO users (name) VALUES (?)', 'Charlie')
      db.run('INSERT INTO users (name) VALUES (?)', 'Alice')
      db.run('INSERT INTO users (name) VALUES (?)', 'Bob')

      const rows = db.query<{ id: number; name: string }>(
        'SELECT * FROM users ORDER BY name',
      )
      expect(rows[0].name).toBe('Alice')
      expect(rows[1].name).toBe('Bob')
      expect(rows[2].name).toBe('Charlie')
    })

    test('query with LIMIT', () => {
      db.exec(
        'CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)',
      )
      db.run('INSERT INTO users (name) VALUES (?)', 'Alice')
      db.run('INSERT INTO users (name) VALUES (?)', 'Bob')
      db.run('INSERT INTO users (name) VALUES (?)', 'Charlie')

      const rows = db.query<{ id: number; name: string }>(
        'SELECT * FROM users LIMIT 2',
      )
      expect(rows.length).toBe(2)
    })

    test('UPDATE modifies rows', () => {
      db.exec(
        'CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)',
      )
      db.run('INSERT INTO users (name) VALUES (?)', 'Alice')

      const result = db.run(
        'UPDATE users SET name = ? WHERE name = ?',
        'Alicia',
        'Alice',
      )
      expect(result.changes).toBe(1)

      const rows = db.query<{ name: string }>('SELECT name FROM users')
      expect(rows[0].name).toBe('Alicia')
    })

    test('DELETE removes rows', () => {
      db.exec(
        'CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)',
      )
      db.run('INSERT INTO users (name) VALUES (?)', 'Alice')
      db.run('INSERT INTO users (name) VALUES (?)', 'Bob')

      const result = db.run('DELETE FROM users WHERE name = ?', 'Alice')
      expect(result.changes).toBe(1)

      const rows = db.query('SELECT * FROM users')
      expect(rows.length).toBe(1)
    })

    test('DROP TABLE removes table', () => {
      db.exec('CREATE TABLE users (id INTEGER PRIMARY KEY)')
      db.exec('DROP TABLE users')

      expect(() => db.query('SELECT * FROM users')).toThrow(
        'No such table: users',
      )
    })

    test('transaction executes function', () => {
      db.exec(
        'CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)',
      )

      const _result = db.transaction((tx) => {
        tx.run('INSERT INTO users (name) VALUES (?)', 'Alice')
        tx.run('INSERT INTO users (name) VALUES (?)', 'Bob')
        return tx.query('SELECT COUNT(*) as count FROM users')
      })

      // Transaction should have completed
      expect(db.inTransaction).toBe(false)
    })
  })

  describe('Statement', () => {
    test('prepare creates statement', () => {
      db.exec('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)')
      const stmt = db.prepare('SELECT * FROM users WHERE id = ?')
      expect(stmt).toBeInstanceOf(Statement)
      expect(stmt.paramsCount).toBe(1)
    })

    test('statement.all returns all rows', () => {
      db.exec(
        'CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)',
      )
      db.run('INSERT INTO users (name) VALUES (?)', 'Alice')
      db.run('INSERT INTO users (name) VALUES (?)', 'Bob')

      const stmt = db.prepare<{ id: number; name: string }>(
        'SELECT * FROM users',
      )
      const rows = stmt.all()
      expect(rows.length).toBe(2)
    })

    test('statement.get returns first row', () => {
      db.exec(
        'CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)',
      )
      db.run('INSERT INTO users (name) VALUES (?)', 'Alice')

      const stmt = db.prepare<{ id: number; name: string }>(
        'SELECT * FROM users',
      )
      const row = stmt.get()
      expect(row?.name).toBe('Alice')
    })

    test('statement.get returns null for no results', () => {
      db.exec('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)')

      const stmt = db.prepare<{ id: number; name: string }>(
        'SELECT * FROM users',
      )
      const row = stmt.get()
      expect(row).toBeNull()
    })

    test('statement.run executes mutation', () => {
      db.exec(
        'CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)',
      )

      const stmt = db.prepare('INSERT INTO users (name) VALUES (?)')
      const result = stmt.run('Alice')
      expect(result.changes).toBe(1)
    })

    test('statement.values returns array of arrays', () => {
      db.exec(
        'CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)',
      )
      db.run('INSERT INTO users (name) VALUES (?)', 'Alice')

      const stmt = db.prepare('SELECT * FROM users')
      const values = stmt.values()
      expect(Array.isArray(values)).toBe(true)
      expect(Array.isArray(values[0])).toBe(true)
    })

    test('statement.finalize prevents further use', () => {
      db.exec('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)')
      const stmt = db.prepare('SELECT * FROM users')
      stmt.finalize()

      expect(() => stmt.all()).toThrow('Statement has been finalized')
    })

    test('statements are cached', () => {
      db.exec('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)')
      const stmt1 = db.prepare('SELECT * FROM users')
      const stmt2 = db.prepare('SELECT * FROM users')
      expect(stmt1).toBe(stmt2)
    })
  })

  describe('Data Types', () => {
    test('handles NULL values', () => {
      db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, value TEXT)')
      db.run('INSERT INTO test (id, value) VALUES (?, ?)', 1, null)

      const rows = db.query<{ id: number; value: string | null }>(
        'SELECT * FROM test',
      )
      expect(rows[0].value).toBeNull()
    })

    test('handles integer values', () => {
      db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, value INTEGER)')
      db.run('INSERT INTO test (id, value) VALUES (?, ?)', 1, 42)

      const rows = db.query<{ id: number; value: number }>('SELECT * FROM test')
      expect(rows[0].value).toBe(42)
    })

    test('handles text values', () => {
      db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, value TEXT)')
      db.run('INSERT INTO test (id, value) VALUES (?, ?)', 1, 'hello world')

      const rows = db.query<{ id: number; value: string }>('SELECT * FROM test')
      expect(rows[0].value).toBe('hello world')
    })

    test('handles text with special characters', () => {
      db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, value TEXT)')
      db.run('INSERT INTO test (id, value) VALUES (?, ?)', 1, "O'Brien")

      const rows = db.query<{ id: number; value: string }>('SELECT * FROM test')
      expect(rows[0].value).toBe("O'Brien")
    })
  })

  describe('Constants', () => {
    test('exports SQLITE_VERSION', () => {
      expect(SQLITE_VERSION).toBe('3.45.0')
    })

    test('exports SQLITE_VERSION_NUMBER', () => {
      expect(SQLITE_VERSION_NUMBER).toBe(3045000)
    })

    test('exports open flags', () => {
      expect(OPEN_READONLY).toBe(1)
      expect(OPEN_READWRITE).toBe(2)
      expect(OPEN_CREATE).toBe(4)
    })

    test('exports status codes', () => {
      expect(SQLITE_OK).toBe(0)
    })
  })

  describe('Error Handling', () => {
    test('throws on unsupported SQL', () => {
      expect(() => db.exec('EXPLAIN SELECT 1')).toThrow('Unsupported SQL')
    })

    test('throws on invalid table', () => {
      expect(() => db.query('SELECT * FROM nonexistent')).toThrow(
        'No such table',
      )
    })

    test('throws on closed database', () => {
      db.close()
      expect(() => db.exec('SELECT 1')).toThrow('Database is closed')
    })
  })
})

describe('SQLit Connection', () => {
  test('parses sqlit:// connection string', () => {
    const db = new Database('sqlit://test-database')
    expect(db.path).toBe('sqlit://test-database')
    expect(db.inMemory).toBe(false)
    db.close()
  })

  test('async methods available for SQLit backend', () => {
    const db = new Database('sqlit://test-database')
    expect(typeof db.queryAsync).toBe('function')
    expect(typeof db.execAsync).toBe('function')
    db.close()
  })

  test('sync methods throw for SQLit backend', () => {
    const db = new Database('sqlit://test-database')
    expect(() => db.exec('SELECT 1')).toThrow('SQLit backend requires async execution')
    expect(() => db.query('SELECT 1')).toThrow('SQLit backend requires async execution')
    db.close()
  })

  test('file-based SQLite throws error (fail-fast)', () => {
    expect(() => new Database('/path/to/database.sqlite')).toThrow(
      "File-based SQLite (/path/to/database.sqlite) is not available in workerd"
    )
    expect(() => new Database('./local.db')).toThrow(
      "File-based SQLite (./local.db) is not available in workerd"
    )
    expect(() => new Database('database.sqlite')).toThrow(
      "File-based SQLite (database.sqlite) is not available in workerd"
    )
  })
})

// ============================================================
// EDGE CASES AND BOUNDARY CONDITIONS
// ============================================================

describe('Edge Cases - Database', () => {
  let db: Database

  beforeEach(() => {
    db = new Database(':memory:')
  })

  afterEach(() => {
    if (db.open) db.close()
  })

  test('create table with IF NOT EXISTS', () => {
    db.exec('CREATE TABLE IF NOT EXISTS test (id INTEGER PRIMARY KEY)')
    db.exec('CREATE TABLE IF NOT EXISTS test (id INTEGER PRIMARY KEY)')
    // Should not throw
    const rows = db.query('SELECT * FROM test')
    expect(rows).toEqual([])
  })

  test('drop table with IF EXISTS', () => {
    db.exec('DROP TABLE IF EXISTS nonexistent')
    // Should not throw
  })

  test('insert with explicit id', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, value TEXT)')
    db.run('INSERT INTO test (id, value) VALUES (?, ?)', 100, 'hundred')
    const rows = db.query<{ id: number; value: string }>('SELECT * FROM test')
    expect(rows[0].id).toBe(100)
  })

  test('insert with NULL value', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, value TEXT)')
    db.run('INSERT INTO test (id, value) VALUES (?, ?)', 1, null)
    const rows = db.query<{ id: number; value: string | null }>('SELECT * FROM test')
    expect(rows[0].value).toBeNull()
  })

  test('select with ORDER BY DESC', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY AUTOINCREMENT, value INTEGER)')
    db.run('INSERT INTO test (value) VALUES (?)', 1)
    db.run('INSERT INTO test (value) VALUES (?)', 3)
    db.run('INSERT INTO test (value) VALUES (?)', 2)
    const rows = db.query<{ value: number }>('SELECT value FROM test ORDER BY value DESC')
    expect(rows[0].value).toBe(3)
    expect(rows[2].value).toBe(1)
  })

  test('select with OFFSET', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY AUTOINCREMENT, value INTEGER)')
    for (let i = 0; i < 10; i++) {
      db.run('INSERT INTO test (value) VALUES (?)', i)
    }
    const rows = db.query<{ value: number }>('SELECT value FROM test LIMIT 3 OFFSET 5')
    expect(rows.length).toBe(3)
    expect(rows[0].value).toBe(5)
  })

  test('update without WHERE affects all rows', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY AUTOINCREMENT, value INTEGER)')
    db.run('INSERT INTO test (value) VALUES (?)', 1)
    db.run('INSERT INTO test (value) VALUES (?)', 2)
    const result = db.run('UPDATE test SET value = ?', 99)
    expect(result.changes).toBe(2)
    const rows = db.query<{ value: number }>('SELECT value FROM test')
    expect(rows.every(r => r.value === 99)).toBe(true)
  })

  test('delete without WHERE removes all rows', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY AUTOINCREMENT, value INTEGER)')
    db.run('INSERT INTO test (value) VALUES (?)', 1)
    db.run('INSERT INTO test (value) VALUES (?)', 2)
    const result = db.run('DELETE FROM test')
    expect(result.changes).toBe(2)
    const rows = db.query('SELECT * FROM test')
    expect(rows.length).toBe(0)
  })

  test('multiple statements with semicolons', () => {
    db.exec(`
      CREATE TABLE a (id INTEGER PRIMARY KEY);
      CREATE TABLE b (id INTEGER PRIMARY KEY);
      CREATE TABLE c (id INTEGER PRIMARY KEY)
    `)
    expect(db.query('SELECT * FROM a')).toEqual([])
    expect(db.query('SELECT * FROM b')).toEqual([])
    expect(db.query('SELECT * FROM c')).toEqual([])
  })

  test('double close does not throw', () => {
    db.close()
    expect(() => db.close()).not.toThrow()
  })

  test('operations after close throw', () => {
    db.close()
    expect(() => db.run('SELECT 1')).toThrow('Database is closed')
    expect(() => db.query('SELECT 1')).toThrow('Database is closed')
    expect(() => db.prepare('SELECT 1')).toThrow('Database is closed')
  })
})

describe('Edge Cases - Statement', () => {
  let db: Database

  beforeEach(() => {
    db = new Database(':memory:')
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY AUTOINCREMENT, value TEXT)')
  })

  afterEach(() => {
    if (db.open) db.close()
  })

  test('statement with multiple parameters', () => {
    const stmt = db.prepare('INSERT INTO test (value) VALUES (?)')
    stmt.run('first')
    stmt.run('second')
    stmt.run('third')
    const rows = db.query('SELECT * FROM test')
    expect(rows.length).toBe(3)
  })

  test('statement.run returns lastInsertRowid', () => {
    const stmt = db.prepare('INSERT INTO test (value) VALUES (?)')
    expect(stmt.run('a').lastInsertRowid).toBe(1)
    expect(stmt.run('b').lastInsertRowid).toBe(2)
    expect(stmt.run('c').lastInsertRowid).toBe(3)
  })

  test('statement.values returns correct structure', () => {
    db.run('INSERT INTO test (value) VALUES (?)', 'test')
    const stmt = db.prepare('SELECT id, value FROM test')
    const values = stmt.values()
    expect(values.length).toBe(1)
    expect(values[0].length).toBe(2) // Two columns
    expect(values[0]).toContain('test')
  })

  test('finalized statement operations throw', () => {
    const stmt = db.prepare('SELECT * FROM test')
    stmt.finalize()
    expect(() => stmt.all()).toThrow('finalized')
    expect(() => stmt.get()).toThrow('finalized')
    expect(() => stmt.run()).toThrow('finalized')
    expect(() => stmt.values()).toThrow('finalized')
  })
})

describe('Edge Cases - Data Types', () => {
  let db: Database

  beforeEach(() => {
    db = new Database(':memory:')
  })

  afterEach(() => {
    if (db.open) db.close()
  })

  test('handles empty string', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, value TEXT)')
    db.run('INSERT INTO test (id, value) VALUES (?, ?)', 1, '')
    const rows = db.query<{ value: string }>('SELECT value FROM test')
    expect(rows[0].value).toBe('')
  })

  test('handles very long text', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, value TEXT)')
    const longText = 'a'.repeat(10000)
    db.run('INSERT INTO test (id, value) VALUES (?, ?)', 1, longText)
    const rows = db.query<{ value: string }>('SELECT value FROM test')
    expect(rows[0].value).toBe(longText)
  })

  test('handles special SQL characters in text', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, value TEXT)')
    db.run('INSERT INTO test (id, value) VALUES (?, ?)', 1, "test's \"value\" with % and _")
    const rows = db.query<{ value: string }>('SELECT value FROM test')
    expect(rows[0].value).toBe("test's \"value\" with % and _")
  })

  test('handles large integer', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, value INTEGER)')
    db.run('INSERT INTO test (id, value) VALUES (?, ?)', 1, 9007199254740991)
    const rows = db.query<{ value: number }>('SELECT value FROM test')
    expect(rows[0].value).toBe(9007199254740991)
  })

  test('handles negative integer', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, value INTEGER)')
    db.run('INSERT INTO test (id, value) VALUES (?, ?)', 1, -42)
    const rows = db.query<{ value: number }>('SELECT value FROM test')
    expect(rows[0].value).toBe(-42)
  })

  test('handles decimal numbers', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, value REAL)')
    db.run('INSERT INTO test (id, value) VALUES (?, ?)', 1, 3.14159)
    const rows = db.query<{ value: number }>('SELECT value FROM test')
    expect(rows[0].value).toBeCloseTo(3.14159, 5)
  })

  test('handles boolean as integer', () => {
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY, flag INTEGER)')
    db.run('INSERT INTO test (id, flag) VALUES (?, ?)', 1, 1)
    db.run('INSERT INTO test (id, flag) VALUES (?, ?)', 2, 0)
    const rows = db.query<{ flag: number }>('SELECT flag FROM test ORDER BY id')
    expect(rows[0].flag).toBe(1) // true
    expect(rows[1].flag).toBe(0) // false
  })
})

describe('Edge Cases - Aggregate Functions', () => {
  let db: Database

  beforeEach(() => {
    db = new Database(':memory:')
    db.exec('CREATE TABLE test (id INTEGER PRIMARY KEY AUTOINCREMENT, value INTEGER)')
    db.run('INSERT INTO test (value) VALUES (?)', 10)
    db.run('INSERT INTO test (value) VALUES (?)', 20)
    db.run('INSERT INTO test (value) VALUES (?)', 30)
  })

  afterEach(() => {
    if (db.open) db.close()
  })

  test('COUNT aggregate', () => {
    const rows = db.query<{ count: number }>('SELECT COUNT(*) as count FROM test')
    expect(rows[0].count).toBe(3)
  })

  test('SUM aggregate', () => {
    const rows = db.query<{ sum: number }>('SELECT SUM(value) as sum FROM test')
    expect(rows[0].sum).toBe(60)
  })

  test('AVG aggregate', () => {
    const rows = db.query<{ avg: number }>('SELECT AVG(value) as avg FROM test')
    expect(rows[0].avg).toBe(20)
  })

  test('MIN aggregate', () => {
    const rows = db.query<{ min: number }>('SELECT MIN(value) as min FROM test')
    expect(rows[0].min).toBe(10)
  })

  test('MAX aggregate', () => {
    const rows = db.query<{ max: number }>('SELECT MAX(value) as max FROM test')
    expect(rows[0].max).toBe(30)
  })

  test('COUNT on empty table', () => {
    db.exec('CREATE TABLE empty (id INTEGER PRIMARY KEY)')
    const rows = db.query<{ count: number }>('SELECT COUNT(*) as count FROM empty')
    expect(rows[0].count).toBe(0)
  })

  test('AVG on empty table returns null', () => {
    db.exec('CREATE TABLE empty (id INTEGER PRIMARY KEY, value INTEGER)')
    const rows = db.query<{ avg: number | null }>('SELECT AVG(value) as avg FROM empty')
    expect(rows[0].avg).toBeNull()
  })
})

describe('Edge Cases - Error Messages', () => {
  let db: Database

  beforeEach(() => {
    db = new Database(':memory:')
  })

  afterEach(() => {
    if (db.open) db.close()
  })

  test('invalid CREATE TABLE syntax', () => {
    expect(() => db.exec('CREATE TABLE')).toThrow()
  })

  test('invalid INSERT syntax', () => {
    db.exec('CREATE TABLE test (id INTEGER)')
    expect(() => db.run('INSERT INTO')).toThrow()
  })

  test('invalid SELECT syntax', () => {
    expect(() => db.query('SELECT FROM')).toThrow()
  })

  test('insert into nonexistent table', () => {
    expect(() => db.run('INSERT INTO missing (id) VALUES (?)', 1)).toThrow()
  })
})

describe('Concurrent Database Operations', () => {
  test('multiple databases are independent', () => {
    const db1 = new Database(':memory:')
    const db2 = new Database(':memory:')

    db1.exec('CREATE TABLE test (id INTEGER PRIMARY KEY)')
    db2.exec('CREATE TABLE other (id INTEGER PRIMARY KEY)')

    expect(db1.query('SELECT * FROM test')).toEqual([])
    expect(db2.query('SELECT * FROM other')).toEqual([])

    expect(() => db1.query('SELECT * FROM other')).toThrow('No such table')
    expect(() => db2.query('SELECT * FROM test')).toThrow('No such table')

    db1.close()
    db2.close()
  })
})

// ============================================================
// SQLit HTTP CLIENT TESTS
// Uses a mock HTTP server to test the HTTP client code path
// ============================================================

describe('SQLit HTTP Client', () => {
  // Mock SQLit server
  let mockServer: ReturnType<typeof Bun.serve> | null = null
  let mockServerPort: number = 0
  let lastInsertId = 0

  // Mock server state (simulated database)
  const mockTables = new Map<string, Array<Record<string, unknown>>>()

  function createMockServer(): Promise<number> {
    return new Promise((resolve) => {
      mockServer = Bun.serve({
        port: 0, // Random available port
        async fetch(req) {
          const url = new URL(req.url)

          // Health endpoint
          if (url.pathname === '/health') {
            return new Response('OK')
          }

          // Query endpoint
          if (url.pathname === '/v1/query') {
            // Simulate 500 error for error-trigger-db
            const dbid = req.headers.get('x-sqlit-database') ?? ''
            if (dbid === 'error-trigger-db') {
              return new Response('Internal Server Error', { status: 500 })
            }
            return handleQuery(req)
          }

          // Exec endpoint
          if (url.pathname === '/v1/exec') {
            // Simulate 500 error for error-trigger-db
            const dbid = req.headers.get('x-sqlit-database') ?? ''
            if (dbid === 'error-trigger-db') {
              return new Response('Internal Server Error', { status: 500 })
            }
            return handleExec(req)
          }

          return new Response('Not Found', { status: 404 })
        },
      })
      mockServerPort = mockServer.port
      resolve(mockServerPort)
    })
  }

  async function handleQuery(req: Request): Promise<Response> {
    const body = await req.json() as { sql: string }
    const sql = body.sql.toUpperCase()

    // Simulate SELECT 1
    if (sql.includes('SELECT 1')) {
      return Response.json({
        success: true,
        rows: [{ test: 1 }],
        rowCount: 1,
        columns: ['test'],
        executionTime: 0,
        blockHeight: 1,
      })
    }

    // Simulate SELECT from non-existent table
    if (sql.includes('DEFINITELY_DOES_NOT_EXIST')) {
      return Response.json({
        success: false,
        error: 'no such table: definitely_does_not_exist_table_12345',
      })
    }

    // Simulate SELECT from mock table
    const selectMatch = sql.match(/SELECT\s+\*\s+FROM\s+(\w+)/i)
    if (selectMatch) {
      const tableName = selectMatch[1].toLowerCase()
      const rows = mockTables.get(tableName) ?? []
      return Response.json({
        success: true,
        rows,
        rowCount: rows.length,
        columns: rows.length > 0 ? Object.keys(rows[0]) : [],
        executionTime: 0,
        blockHeight: 1,
      })
    }

    return Response.json({ success: true, rows: [], rowCount: 0, columns: [], executionTime: 0, blockHeight: 1 })
  }

  async function handleExec(req: Request): Promise<Response> {
    const body = await req.json() as { sql: string }
    const sql = body.sql.toUpperCase()

    // Simulate CREATE TABLE
    if (sql.includes('CREATE TABLE')) {
      const match = sql.match(/CREATE TABLE[^(]*(\w+)/i)
      if (match) {
        const tableName = match[1].toLowerCase()
        if (!mockTables.has(tableName)) {
          mockTables.set(tableName, [])
        }
      }
      return Response.json({
        success: true,
        rowsAffected: 0,
        lastInsertId: '0',
        txHash: '0x0000000000000000000000000000000000000000000000000000000000000001',
        gasUsed: '21000',
        executionTime: 1,
        blockHeight: 1,
      })
    }

    // Simulate INSERT
    if (sql.includes('INSERT INTO')) {
      lastInsertId++
      return Response.json({
        success: true,
        rowsAffected: 1,
        lastInsertId: String(lastInsertId),
        txHash: '0x0000000000000000000000000000000000000000000000000000000000000002',
        gasUsed: '21000',
        executionTime: 1,
        blockHeight: 2,
      })
    }

    // Simulate DELETE
    if (sql.includes('DELETE FROM')) {
      return Response.json({
        success: true,
        rowsAffected: 1,
        lastInsertId: '0',
        txHash: '0x0000000000000000000000000000000000000000000000000000000000000003',
        gasUsed: '21000',
        executionTime: 1,
        blockHeight: 3,
      })
    }

    // Simulate UPDATE
    if (sql.includes('UPDATE')) {
      return Response.json({
        success: true,
        rowsAffected: 1,
        lastInsertId: '0',
        txHash: '0x0000000000000000000000000000000000000000000000000000000000000004',
        gasUsed: '21000',
        executionTime: 1,
        blockHeight: 4,
      })
    }

    return Response.json({ success: true, rowsAffected: 0, lastInsertId: '0', executionTime: 0, blockHeight: 0 })
  }

  beforeAll(async () => {
    await createMockServer()
  })

  afterAll(() => {
    mockServer?.stop()
    mockServer = null
    mockTables.clear()
  })

  beforeEach(() => {
    mockTables.clear()
    lastInsertId = 0
  })

  test('SQLit connection string parsing', () => {
    const db = new Database('sqlit://my-test-database')
    expect(db.path).toBe('sqlit://my-test-database')
    expect(db.inMemory).toBe(false)
    db.close()
  })

  test('HTTP URL connection string parsing', () => {
    const db = new Database('http://localhost:4661/my-db')
    expect(db.path).toBe('http://localhost:4661/my-db')
    expect(db.inMemory).toBe(false)
    db.close()
  })

  test('sync methods throw with helpful message for SQLit', () => {
    const db = new Database('sqlit://test')

    const expectedMessage = 'SQLit backend requires async execution'
    expect(() => db.exec('CREATE TABLE test (id INTEGER)')).toThrow(expectedMessage)
    expect(() => db.run('INSERT INTO test VALUES (1)')).toThrow(expectedMessage)
    expect(() => db.query('SELECT 1')).toThrow(expectedMessage)

    db.close()
  })

  test('async methods exist and are callable', () => {
    const db = new Database('sqlit://test')

    expect(typeof db.queryAsync).toBe('function')
    expect(typeof db.execAsync).toBe('function')

    db.close()
  })

  test('queryAsync connects to mock SQLit server', async () => {
    const db = new Database(`http://localhost:${mockServerPort}/test-db`)

    const rows = await db.queryAsync('SELECT 1 as test')
    expect(Array.isArray(rows)).toBe(true)
    expect(rows.length).toBe(1)
    expect(rows[0]).toEqual({ test: 1 })

    db.close()
  })

  test('execAsync returns changes and lastInsertRowid', async () => {
    const db = new Database(`http://localhost:${mockServerPort}/test-db`)

    // Create a test table
    await db.execAsync('CREATE TABLE IF NOT EXISTS sqlit_test (id INTEGER PRIMARY KEY, value TEXT)')

    // Insert a row
    const result = await db.execAsync("INSERT INTO sqlit_test (value) VALUES ('test-value')")

    // Result should have proper structure (SQLiteRunResult)
    expect(typeof result.changes).toBe('number')
    expect(result.changes).toBe(1)
    // lastInsertRowid can be number or bigint depending on implementation
    expect(Number(result.lastInsertRowid)).toBe(1)

    // Insert another row
    const result2 = await db.execAsync("INSERT INTO sqlit_test (value) VALUES ('test-value-2')")
    expect(Number(result2.lastInsertRowid)).toBe(2)

    db.close()
  })

  test('handles SQLit server errors gracefully', async () => {
    const db = new Database(`http://localhost:${mockServerPort}/test-db`)

    // This should fail because the table doesn't exist
    await expect(db.queryAsync('SELECT * FROM definitely_does_not_exist_table_12345'))
      .rejects.toThrow('no such table')

    db.close()
  })

  test('closed database rejects async operations', async () => {
    const db = new Database('sqlit://test')
    db.close()

    await expect(db.queryAsync('SELECT 1')).rejects.toThrow('Database is closed')
    await expect(db.execAsync('SELECT 1')).rejects.toThrow('Database is closed')
  })

  test('execAsync handles UPDATE correctly', async () => {
    const db = new Database(`http://localhost:${mockServerPort}/test-db`)

    const result = await db.execAsync('UPDATE sqlit_test SET value = ? WHERE id = ?', ['new-value', 1])
    expect(result.changes).toBe(1)

    db.close()
  })

  test('execAsync handles DELETE correctly', async () => {
    const db = new Database(`http://localhost:${mockServerPort}/test-db`)

    const result = await db.execAsync('DELETE FROM sqlit_test WHERE id = ?', [1])
    expect(result.changes).toBe(1)

    db.close()
  })

  test('queryAsync returns empty array for empty table', async () => {
    const db = new Database(`http://localhost:${mockServerPort}/test-db`)

    const rows = await db.queryAsync('SELECT * FROM empty_table')
    expect(Array.isArray(rows)).toBe(true)
    expect(rows.length).toBe(0)

    db.close()
  })

  test('connection via sqlit:// URL format', async () => {
    // This test verifies that sqlit:// URLs use the environment config
    // Since we can't easily test this without modifying env, we just verify the path parsing
    const db = new Database('sqlit://production-db')
    expect(db.path).toBe('sqlit://production-db')
    expect(db.inMemory).toBe(false)
    db.close()
  })

  test('handles HTTP error status codes', async () => {
    // Use error-trigger path that the mock server recognizes
    const db = new Database(`http://localhost:${mockServerPort}/error-trigger-db`)

    await expect(db.queryAsync('SELECT 1')).rejects.toThrow('SQLit request failed: 500')
    await expect(db.execAsync('SELECT 1')).rejects.toThrow('SQLit request failed: 500')

    db.close()
  })
})
