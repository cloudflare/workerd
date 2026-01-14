import { describe, expect, test } from 'bun:test'
import { Database, Statement, SQLITE_VERSION } from './sqlite'

describe('bun:sqlite (unavailable in workerd)', () => {
  test('Database constructor throws', () => {
    expect(() => new Database()).toThrow('not available in workerd')
    expect(() => new Database(':memory:')).toThrow('not available in workerd')
    expect(() => new Database('/tmp/test.db')).toThrow('not available in workerd')
  })

  test('Statement constructor throws', () => {
    expect(() => new Statement()).toThrow('not available in workerd')
  })

  test('exports version constants', () => {
    expect(SQLITE_VERSION).toBe('unavailable')
  })
})
