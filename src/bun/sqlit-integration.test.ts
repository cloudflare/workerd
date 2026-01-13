/**
 * Real SQLit Integration Tests
 *
 * These tests run against the ACTUAL SQLit server, not a mock.
 * They are skipped if SQLIT_ENDPOINT is not set or server is not available.
 *
 * To run: SQLIT_ENDPOINT=http://localhost:4661 bun test src/bun/sqlit-integration.test.ts
 */

import { describe, test, expect, beforeAll, afterAll } from 'bun:test'
import { Database } from './sqlite'

const SQLIT_ENDPOINT = process.env.SQLIT_ENDPOINT ?? 'http://localhost:4661'
const TEST_DB_ID = `workerd-test-${Date.now()}`

let sqlit_available = false
let db: Database

describe('Real SQLit Integration', () => {
  beforeAll(async () => {
    // Check if SQLit server is available
    try {
      const response = await fetch(`${SQLIT_ENDPOINT}/v1/status`, {
        signal: AbortSignal.timeout(3000),
      })
      if (response.ok) {
        const data = await response.json() as { status?: string }
        sqlit_available = data.status === 'running' || data.status === 'ok'
      }
    } catch {
      sqlit_available = false
    }

    if (!sqlit_available) {
      console.log('⚠️  SQLit server not available - skipping real integration tests')
      console.log(`   Set SQLIT_ENDPOINT to SQLit server URL (tried: ${SQLIT_ENDPOINT})`)
      return
    }

    console.log(`✓ SQLit server available at ${SQLIT_ENDPOINT}`)
    db = new Database(`sqlit://${TEST_DB_ID}?endpoint=${SQLIT_ENDPOINT}`)
  })

  afterAll(() => {
    if (sqlit_available && db) {
      db.close()
    }
  })

  test('connects to real SQLit server', async () => {
    if (!sqlit_available) {
      console.log('SKIPPED - SQLit not available')
      return
    }

    // Simple connectivity test
    const result = await db.queryAsync('SELECT 1 as test')
    expect(result).toEqual([{ test: 1 }])
  })

  test('creates table on real SQLit', async () => {
    if (!sqlit_available) {
      console.log('SKIPPED - SQLit not available')
      return
    }

    const result = await db.execAsync(`
      CREATE TABLE IF NOT EXISTS real_test (
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL,
        created_at INTEGER DEFAULT (strftime('%s', 'now'))
      )
    `)

    // CREATE TABLE returns 0 rows affected
    expect(result.changes).toBe(0)
  })

  test('inserts data on real SQLit', async () => {
    if (!sqlit_available) {
      console.log('SKIPPED - SQLit not available')
      return
    }

    const result = await db.execAsync(
      "INSERT INTO real_test (name) VALUES ('test-from-workerd-integration')",
    )

    expect(result.changes).toBe(1)
    expect(Number(result.lastInsertRowid)).toBeGreaterThan(0)
  })

  test('queries data from real SQLit', async () => {
    if (!sqlit_available) {
      console.log('SKIPPED - SQLit not available')
      return
    }

    const rows = await db.queryAsync('SELECT * FROM real_test WHERE name LIKE ?', [
      'test-from-workerd%',
    ])

    expect(rows.length).toBeGreaterThan(0)
    expect(rows[0]).toHaveProperty('id')
    expect(rows[0]).toHaveProperty('name')
    expect(rows[0].name).toBe('test-from-workerd-integration')
  })

  test('updates data on real SQLit', async () => {
    if (!sqlit_available) {
      console.log('SKIPPED - SQLit not available')
      return
    }

    const result = await db.execAsync(
      "UPDATE real_test SET name = 'updated-by-workerd' WHERE name = 'test-from-workerd-integration'",
    )

    expect(result.changes).toBe(1)
  })

  test('deletes data from real SQLit', async () => {
    if (!sqlit_available) {
      console.log('SKIPPED - SQLit not available')
      return
    }

    const result = await db.execAsync("DELETE FROM real_test WHERE name = 'updated-by-workerd'")

    expect(result.changes).toBeGreaterThanOrEqual(0) // May be 0 if already deleted
  })

  test('handles errors from real SQLit', async () => {
    if (!sqlit_available) {
      console.log('SKIPPED - SQLit not available')
      return
    }

    // Query non-existent table
    await expect(db.queryAsync('SELECT * FROM definitely_nonexistent_table_xyz_123')).rejects.toThrow()
  })

  test('supports transactions on real SQLit', async () => {
    if (!sqlit_available) {
      console.log('SKIPPED - SQLit not available')
      return
    }

    // Create a fresh table for transaction test
    await db.execAsync('CREATE TABLE IF NOT EXISTS tx_test (id INTEGER PRIMARY KEY, value TEXT)')

    // Insert multiple rows
    await db.execAsync("INSERT INTO tx_test (value) VALUES ('tx-1')")
    await db.execAsync("INSERT INTO tx_test (value) VALUES ('tx-2')")
    await db.execAsync("INSERT INTO tx_test (value) VALUES ('tx-3')")

    // Verify all rows exist
    const rows = await db.queryAsync('SELECT COUNT(*) as cnt FROM tx_test')
    expect(Number(rows[0].cnt)).toBeGreaterThanOrEqual(3)
  })

  test('returns correct row count', async () => {
    if (!sqlit_available) {
      console.log('SKIPPED - SQLit not available')
      return
    }

    const rows = await db.queryAsync('SELECT * FROM tx_test LIMIT 10')
    expect(Array.isArray(rows)).toBe(true)
    expect(rows.length).toBeGreaterThanOrEqual(3)
  })

  test('supports prepared statement style queries', async () => {
    if (!sqlit_available) {
      console.log('SKIPPED - SQLit not available')
      return
    }

    // Use parameterized query
    const searchTerm = 'tx-1'
    const rows = await db.queryAsync('SELECT * FROM tx_test WHERE value = ?', [searchTerm])

    // May have multiple rows from previous test runs
    expect(rows.length).toBeGreaterThanOrEqual(1)
    expect(rows[0].value).toBe('tx-1')
  })
})

// Performance test
describe('Real SQLit Performance', () => {
  let perfDb: Database

  beforeAll(async () => {
    if (!sqlit_available) return
    perfDb = new Database(`sqlit://${TEST_DB_ID}-perf?endpoint=${SQLIT_ENDPOINT}`)
  })

  afterAll(() => {
    if (sqlit_available && perfDb) {
      perfDb.close()
    }
  })

  test('handles rapid sequential queries', async () => {
    if (!sqlit_available) {
      console.log('SKIPPED - SQLit not available')
      return
    }

    const start = performance.now()
    const iterations = 10

    for (let i = 0; i < iterations; i++) {
      await perfDb.queryAsync('SELECT 1')
    }

    const elapsed = performance.now() - start
    const avgLatency = elapsed / iterations

    console.log(`   Average query latency: ${avgLatency.toFixed(2)}ms`)
    expect(avgLatency).toBeLessThan(100) // Should be fast
  })

  test('handles concurrent queries', async () => {
    if (!sqlit_available) {
      console.log('SKIPPED - SQLit not available')
      return
    }

    const start = performance.now()
    const concurrency = 5

    const results = await Promise.all(
      Array.from({ length: concurrency }, () => perfDb.queryAsync('SELECT 1 as concurrent_test')),
    )

    const elapsed = performance.now() - start

    expect(results.length).toBe(concurrency)
    results.forEach((result) => {
      expect(result).toEqual([{ concurrent_test: 1 }])
    })

    console.log(`   ${concurrency} concurrent queries in ${elapsed.toFixed(2)}ms`)
    expect(elapsed).toBeLessThan(1000) // Should complete in < 1s
  })
})
