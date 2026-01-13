// Copyright (c) 2024 Jeju Network
// Bun SQLite Compatibility Layer for Workerd
// Licensed under the Apache 2.0 license

/**
 * bun:sqlite Module
 *
 * Provides SQLite database functionality compatible with Bun's SQLite API.
 * In workerd/DWS, this is backed by SQLit - our decentralized SQLite service.
 *
 * Usage:
 * - For in-memory databases (default): Uses local in-memory storage
 * - For persistent databases: Connects to SQLit via HTTP API
 *
 * @example
 * ```typescript
 * import { Database } from 'bun:sqlite';
 *
 * // Local in-memory database
 * const db = new Database(':memory:');
 *
 * // Connect to SQLit (DWS)
 * const db = new Database('sqlit://my-database-id');
 * ```
 */

import { ERR_SQLITE_ERROR } from 'bun-internal:errors'

// =============================================================================
// Types
// =============================================================================

export type SQLiteValue = string | number | bigint | Uint8Array | null
export type SQLiteRow = Record<string, SQLiteValue>

export interface SQLiteQueryOptions {
  expand?: boolean
  bigint?: boolean
}

export interface SQLiteRunResult {
  changes: number
  lastInsertRowid: number | bigint
}

export interface SQLiteStatement<T = SQLiteRow> {
  run(...params: SQLiteValue[]): SQLiteRunResult
  all(...params: SQLiteValue[]): T[]
  get(...params: SQLiteValue[]): T | null
  values(...params: SQLiteValue[]): SQLiteValue[][]
  finalize(): void
  readonly columnNames: string[]
  readonly paramsCount: number
}

export type SQLiteTransactionFunction<T> = (db: Database) => T

export interface SQLiteDatabaseOptions {
  readonly?: boolean
  create?: boolean
  strict?: boolean
}

// =============================================================================
// SQLit Connection Configuration
// =============================================================================

interface SQLitConfig {
  endpoint: string
  dbid: string
  timeout: number
  debug: boolean
}

// Helper to get environment variables in workerd-compatible way
function getEnv(name: string): string | undefined {
  if (typeof globalThis !== 'undefined') {
    const proc = (globalThis as Record<string, unknown>).process as
      | { env?: Record<string, string> }
      | undefined
    return proc?.env?.[name]
  }
  return undefined
}

function getSQLitConfigFromEnv(): SQLitConfig {
  const endpoint =
    getEnv('SQLIT_ENDPOINT') ??
    getEnv('SQLIT_BLOCK_PRODUCER_ENDPOINT') ??
    getEnv('SQLIT_URL') ??
    'http://localhost:4661'

  const dbid = getEnv('SQLIT_DATABASE_ID') ?? 'default'
  const timeout = parseInt(getEnv('SQLIT_TIMEOUT') ?? '30000', 10)
  const debug = getEnv('SQLIT_DEBUG') === 'true'

  return { endpoint, dbid, timeout, debug }
}

// =============================================================================
// SQLit HTTP Client
// =============================================================================

// Real SQLit API response format
interface SQLitQueryResponse {
  success: boolean
  // Query response
  rows?: Record<string, unknown>[]
  rowCount?: number
  columns?: string[]
  // Exec response
  rowsAffected?: number
  lastInsertId?: string  // SQLit returns string, not number
  txHash?: string
  gasUsed?: string
  // Common
  executionTime?: number
  blockHeight?: number
  // Error case
  error?: string
  message?: string
}

class SQLitHttpClient {
  private readonly endpoint: string
  private readonly dbid: string
  private readonly timeout: number
  private readonly debug: boolean

  constructor(config: SQLitConfig) {
    this.endpoint = config.endpoint
    this.dbid = config.dbid
    this.timeout = config.timeout
    this.debug = config.debug
  }

  async query(sql: string, params: SQLiteValue[] = []): Promise<SQLiteRow[]> {
    const formattedSql = this.formatSQL(sql, params)
    return this.fetch('query', formattedSql)
  }

  async exec(
    sql: string,
    params: SQLiteValue[] = [],
  ): Promise<{ rowsAffected: number; lastInsertId: number | bigint }> {
    const formattedSql = this.formatSQL(sql, params)
    return this.fetchExec(formattedSql)
  }

  async execRaw(sql: string): Promise<void> {
    await this.fetch('exec', sql)
  }

  private formatSQL(sql: string, params: SQLiteValue[]): string {
    if (params.length === 0) return sql

    let paramIndex = 0
    return sql.replace(/\?/g, () => {
      const param = params[paramIndex++]
      if (param === null) return 'NULL'
      if (typeof param === 'string') return `'${param.replace(/'/g, "''")}'`
      if (typeof param === 'number') return String(param)
      if (typeof param === 'bigint') return String(param)
      if (param instanceof Uint8Array) return `X'${this.bytesToHex(param)}'`
      return 'NULL'
    })
  }

  private bytesToHex(bytes: Uint8Array): string {
    return Array.from(bytes)
      .map((b) => b.toString(16).padStart(2, '0'))
      .join('')
  }

  private async fetch(
    method: 'query' | 'exec',
    sql: string,
  ): Promise<SQLiteRow[]> {
    const uri = `${this.endpoint}/v1/${method}`

    if (this.debug) {
      console.log(`[bun:sqlite] ${method}: ${sql}`)
    }

    const response = await fetch(uri, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-sqlit-database': this.dbid,
      },
      body: JSON.stringify({ sql }),
      signal: AbortSignal.timeout(this.timeout),
    })

    if (!response.ok) {
      throw new ERR_SQLITE_ERROR(`SQLit request failed: ${response.status}`)
    }

    const result: SQLitQueryResponse = await response.json()

    if (!result.success) {
      throw new ERR_SQLITE_ERROR(result.error ?? result.message ?? 'Unknown SQLit error')
    }

    return (result.rows as SQLiteRow[]) ?? []
  }

  private async fetchExec(
    sql: string,
  ): Promise<{ rowsAffected: number; lastInsertId: number | bigint }> {
    const uri = `${this.endpoint}/v1/exec`

    if (this.debug) {
      console.log(`[bun:sqlite] exec: ${sql}`)
    }

    const response = await fetch(uri, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-sqlit-database': this.dbid,
      },
      body: JSON.stringify({ sql }),
      signal: AbortSignal.timeout(this.timeout),
    })

    if (!response.ok) {
      throw new ERR_SQLITE_ERROR(`SQLit request failed: ${response.status}`)
    }

    const result: SQLitQueryResponse = await response.json()

    if (!result.success) {
      throw new ERR_SQLITE_ERROR(result.error ?? result.message ?? 'Unknown SQLit error')
    }

    // Real SQLit returns: { success, rowsAffected, lastInsertId (string), txHash, ... }
    const rowsAffected = result.rowsAffected ?? 0
    const lastInsertId = result.lastInsertId ? BigInt(result.lastInsertId) : 0n

    return { rowsAffected, lastInsertId }
  }
}

// =============================================================================
// In-Memory Storage (for :memory: databases)
// =============================================================================

interface Table {
  name: string
  columns: ColumnDef[]
  rows: SQLiteRow[]
  autoIncrement: number
}

interface ColumnDef {
  name: string
  type: string
  primaryKey: boolean
  autoIncrement: boolean
  notNull: boolean
  defaultValue: SQLiteValue
}

class InMemoryStorage {
  private tables = new Map<string, Table>()

  createTable(name: string, columns: ColumnDef[]): void {
    if (this.tables.has(name)) return
    this.tables.set(name, {
      name,
      columns,
      rows: [],
      autoIncrement: 1,
    })
  }

  dropTable(name: string): void {
    this.tables.delete(name)
  }

  insert(
    tableName: string,
    columns: string[],
    values: SQLiteValue[],
  ): { lastInsertRowid: number } {
    const table = this.tables.get(tableName)
    if (!table) throw new ERR_SQLITE_ERROR(`No such table: ${tableName}`)

    const row: SQLiteRow = {}
    const tableColumns =
      columns.length > 0 ? columns : table.columns.map((c) => c.name)

    for (let i = 0; i < tableColumns.length; i++) {
      row[tableColumns[i]] = values[i] ?? null
    }

    // Handle auto-increment primary key
    const pkCol = table.columns.find((c) => c.primaryKey && c.autoIncrement)
    let lastInsertRowid = table.autoIncrement

    if (pkCol) {
      // If primary key column value is not provided or is null, auto-increment
      if (
        !(pkCol.name in row) ||
        row[pkCol.name] === null ||
        row[pkCol.name] === undefined
      ) {
        row[pkCol.name] = table.autoIncrement
        lastInsertRowid = table.autoIncrement
        table.autoIncrement++
      } else {
        // Use provided value and update auto-increment if higher
        const providedId = row[pkCol.name] as number
        lastInsertRowid = providedId
        if (providedId >= table.autoIncrement) {
          table.autoIncrement = providedId + 1
        }
      }
    } else {
      // No auto-increment PK, just use row count as rowid
      lastInsertRowid = table.rows.length + 1
      table.autoIncrement++
    }

    // Add rowid if not present
    if (!('rowid' in row)) {
      row.rowid = lastInsertRowid
    }

    table.rows.push(row)
    return { lastInsertRowid }
  }

  select(
    tableName: string,
    columns: string[] | '*',
    where?: { column: string; value: SQLiteValue },
    orderBy?: { column: string; desc: boolean },
    limit?: number,
    offset?: number,
    aggregates?: AggregateColumn[],
  ): SQLiteRow[] {
    const table = this.tables.get(tableName)
    if (!table) throw new ERR_SQLITE_ERROR(`No such table: ${tableName}`)

    let rows = [...table.rows]

    // Apply WHERE
    if (where) {
      rows = rows.filter((row) => row[where.column] === where.value)
    }

    // Handle aggregates
    if (aggregates && aggregates.length > 0) {
      const result: SQLiteRow = {}

      for (const agg of aggregates) {
        const alias = agg.alias ?? agg.fn.toLowerCase()

        switch (agg.fn) {
          case 'COUNT':
            result[alias] = rows.length
            break

          case 'SUM': {
            let sum = 0
            for (const row of rows) {
              const val = row[agg.column]
              if (typeof val === 'number') sum += val
              else if (typeof val === 'bigint') sum += Number(val)
            }
            result[alias] = sum
            break
          }

          case 'AVG': {
            let sum = 0
            let count = 0
            for (const row of rows) {
              const val = row[agg.column]
              if (typeof val === 'number') {
                sum += val
                count++
              } else if (typeof val === 'bigint') {
                sum += Number(val)
                count++
              }
            }
            result[alias] = count > 0 ? sum / count : null
            break
          }

          case 'MIN': {
            let minVal: SQLiteValue = null
            for (const row of rows) {
              const val = row[agg.column]
              if (val !== null && (minVal === null || val < minVal)) {
                minVal = val
              }
            }
            result[alias] = minVal
            break
          }

          case 'MAX': {
            let maxVal: SQLiteValue = null
            for (const row of rows) {
              const val = row[agg.column]
              if (val !== null && (maxVal === null || val > maxVal)) {
                maxVal = val
              }
            }
            result[alias] = maxVal
            break
          }
        }
      }

      return [result]
    }

    // Apply ORDER BY
    if (orderBy) {
      rows.sort((a, b) => {
        const aVal = a[orderBy.column]
        const bVal = b[orderBy.column]
        if (aVal === bVal) return 0
        if (aVal === null) return orderBy.desc ? -1 : 1
        if (bVal === null) return orderBy.desc ? 1 : -1
        const cmp = aVal < bVal ? -1 : 1
        return orderBy.desc ? -cmp : cmp
      })
    }

    // Apply OFFSET
    if (offset !== undefined) {
      rows = rows.slice(offset)
    }

    // Apply LIMIT
    if (limit !== undefined) {
      rows = rows.slice(0, limit)
    }

    // Apply column selection
    if (columns !== '*' && columns.length > 0) {
      rows = rows.map((row) => {
        const selected: SQLiteRow = {}
        for (const col of columns) {
          selected[col] = row[col]
        }
        return selected
      })
    }

    return rows
  }

  update(
    tableName: string,
    set: Record<string, SQLiteValue>,
    where?: { column: string; value: SQLiteValue },
  ): number {
    const table = this.tables.get(tableName)
    if (!table) throw new ERR_SQLITE_ERROR(`No such table: ${tableName}`)

    let changes = 0
    for (const row of table.rows) {
      if (!where || row[where.column] === where.value) {
        for (const [col, val] of Object.entries(set)) {
          row[col] = val
        }
        changes++
      }
    }
    return changes
  }

  delete(
    tableName: string,
    where?: { column: string; value: SQLiteValue },
  ): number {
    const table = this.tables.get(tableName)
    if (!table) throw new ERR_SQLITE_ERROR(`No such table: ${tableName}`)

    if (!where) {
      const count = table.rows.length
      table.rows = []
      return count
    }

    const initialLength = table.rows.length
    table.rows = table.rows.filter((row) => row[where.column] !== where.value)
    return initialLength - table.rows.length
  }

  clear(): void {
    this.tables.clear()
  }
}

// =============================================================================
// SQL Parser (for in-memory databases)
// =============================================================================

interface ParsedCreateTable {
  type: 'CREATE_TABLE'
  tableName: string
  columns: ColumnDef[]
  ifNotExists: boolean
}

interface ParsedDropTable {
  type: 'DROP_TABLE'
  tableName: string
  ifExists: boolean
}

interface ParsedInsert {
  type: 'INSERT'
  tableName: string
  columns: string[]
  values: SQLiteValue[]
}

interface AggregateColumn {
  fn: 'COUNT' | 'SUM' | 'AVG' | 'MIN' | 'MAX'
  column: string // '*' for COUNT(*)
  alias?: string
}

interface ParsedSelect {
  type: 'SELECT'
  tableName: string
  columns: string[] | '*'
  aggregates?: AggregateColumn[]
  where?: { column: string; value: SQLiteValue }
  orderBy?: { column: string; desc: boolean }
  limit?: number
  offset?: number
}

interface ParsedUpdate {
  type: 'UPDATE'
  tableName: string
  set: Record<string, SQLiteValue>
  where?: { column: string; value: SQLiteValue }
}

interface ParsedDelete {
  type: 'DELETE'
  tableName: string
  where?: { column: string; value: SQLiteValue }
}

interface ParsedPragma {
  type: 'PRAGMA'
  name: string
  value?: string
}

type ParsedStatement =
  | ParsedCreateTable
  | ParsedDropTable
  | ParsedInsert
  | ParsedSelect
  | ParsedUpdate
  | ParsedDelete
  | ParsedPragma

function parseSQL(sql: string, params: SQLiteValue[]): ParsedStatement {
  const normalized = sql.trim()
  const upper = normalized.toUpperCase()

  if (upper.startsWith('CREATE TABLE')) {
    return parseCreateTable(normalized)
  }
  if (upper.startsWith('DROP TABLE')) {
    return parseDropTable(normalized)
  }
  if (upper.startsWith('INSERT')) {
    return parseInsert(normalized, params)
  }
  if (upper.startsWith('SELECT')) {
    return parseSelect(normalized, params)
  }
  if (upper.startsWith('UPDATE')) {
    return parseUpdate(normalized, params)
  }
  if (upper.startsWith('DELETE')) {
    return parseDelete(normalized, params)
  }
  if (upper.startsWith('PRAGMA')) {
    return parsePragma(normalized)
  }

  throw new ERR_SQLITE_ERROR(`Unsupported SQL: ${sql}`)
}

function parseCreateTable(sql: string): ParsedCreateTable {
  const ifNotExists = /IF\s+NOT\s+EXISTS/i.test(sql)
  const match = sql.match(
    /CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?["']?(\w+)["']?\s*\(([^)]+)\)/i,
  )
  if (!match) throw new ERR_SQLITE_ERROR(`Invalid CREATE TABLE: ${sql}`)

  const tableName = match[1]
  const columnsDef = match[2]

  const columns: ColumnDef[] = []
  const parts = columnsDef.split(',')

  for (const part of parts) {
    const trimmed = part.trim()
    if (
      trimmed.toUpperCase().startsWith('PRIMARY KEY') ||
      trimmed.toUpperCase().startsWith('FOREIGN KEY') ||
      trimmed.toUpperCase().startsWith('UNIQUE') ||
      trimmed.toUpperCase().startsWith('CHECK')
    ) {
      continue // Skip constraint definitions
    }

    const tokens = trimmed.split(/\s+/)
    const name = tokens[0].replace(/["'`]/g, '')
    const type = tokens[1]?.toUpperCase() ?? 'TEXT'
    const isPrimaryKey = /PRIMARY\s+KEY/i.test(trimmed)
    const isAutoIncrement = /AUTOINCREMENT/i.test(trimmed)
    const isNotNull = /NOT\s+NULL/i.test(trimmed)

    columns.push({
      name,
      type,
      primaryKey: isPrimaryKey,
      autoIncrement: isAutoIncrement,
      notNull: isNotNull,
      defaultValue: null,
    })
  }

  return { type: 'CREATE_TABLE', tableName, columns, ifNotExists }
}

function parseDropTable(sql: string): ParsedDropTable {
  const ifExists = /IF\s+EXISTS/i.test(sql)
  const match = sql.match(/DROP\s+TABLE\s+(?:IF\s+EXISTS\s+)?["']?(\w+)["']?/i)
  if (!match) throw new ERR_SQLITE_ERROR(`Invalid DROP TABLE: ${sql}`)
  return { type: 'DROP_TABLE', tableName: match[1], ifExists }
}

function parseInsert(sql: string, params: SQLiteValue[]): ParsedInsert {
  const match = sql.match(
    /INSERT\s+(?:OR\s+\w+\s+)?INTO\s+["']?(\w+)["']?\s*(?:\(([^)]+)\))?\s*VALUES\s*\(([^)]+)\)/i,
  )
  if (!match) throw new ERR_SQLITE_ERROR(`Invalid INSERT: ${sql}`)

  const tableName = match[1]
  const columns =
    match[2]?.split(',').map((c) => c.trim().replace(/["'`]/g, '')) ?? []
  const valueTemplate = match[3]

  const values: SQLiteValue[] = []
  let paramIndex = 0
  const valueParts = valueTemplate.split(',').map((v) => v.trim())

  for (const part of valueParts) {
    if (part === '?') {
      values.push(params[paramIndex++])
    } else if (part.startsWith('?')) {
      const idx = parseInt(part.slice(1), 10) - 1
      values.push(params[idx])
    } else {
      values.push(parseLiteral(part))
    }
  }

  return { type: 'INSERT', tableName, columns, values }
}

function parseSelect(sql: string, params: SQLiteValue[]): ParsedSelect {
  const match = sql.match(
    /SELECT\s+(.+?)\s+FROM\s+["']?(\w+)["']?(?:\s+WHERE\s+(.+?))?(?:\s+ORDER\s+BY\s+(\w+)(?:\s+(ASC|DESC))?)?(?:\s+LIMIT\s+(\d+))?(?:\s+OFFSET\s+(\d+))?$/i,
  )
  if (!match) throw new ERR_SQLITE_ERROR(`Invalid SELECT: ${sql}`)

  const columnsStr = match[1].trim()
  const tableName = match[2]

  // Parse columns and detect aggregates
  const aggregates: AggregateColumn[] = []
  const regularColumns: string[] = []

  if (columnsStr === '*') {
    // Simple wildcard select
  } else {
    const colParts = columnsStr.split(',').map((c) => c.trim())

    for (const part of colParts) {
      // Check for aggregate functions: COUNT(*), COUNT(col), SUM(col), AVG(col), MIN(col), MAX(col)
      const aggMatch = part.match(
        /^(COUNT|SUM|AVG|MIN|MAX)\s*\(\s*(\*|\w+)\s*\)(?:\s+(?:AS\s+)?["']?(\w+)["']?)?$/i,
      )
      if (aggMatch) {
        aggregates.push({
          fn: aggMatch[1].toUpperCase() as AggregateColumn['fn'],
          column: aggMatch[2],
          alias: aggMatch[3] ?? aggMatch[1].toLowerCase(),
        })
      } else {
        regularColumns.push(part.replace(/["'`]/g, ''))
      }
    }
  }

  const columns =
    columnsStr === '*' ? '*' : regularColumns.length > 0 ? regularColumns : []

  let where: { column: string; value: SQLiteValue } | undefined
  if (match[3]) {
    const whereMatch = match[3].match(/["']?(\w+)["']?\s*=\s*(.+)/i)
    if (whereMatch) {
      const value = whereMatch[2].trim()
      where = {
        column: whereMatch[1],
        value: value === '?' ? params[0] : parseLiteral(value),
      }
    }
  }

  const orderBy = match[4]
    ? { column: match[4], desc: match[5]?.toUpperCase() === 'DESC' }
    : undefined

  const limit = match[6] ? parseInt(match[6], 10) : undefined
  const offset = match[7] ? parseInt(match[7], 10) : undefined

  return {
    type: 'SELECT',
    tableName,
    columns,
    aggregates: aggregates.length > 0 ? aggregates : undefined,
    where,
    orderBy,
    limit,
    offset,
  }
}

function parseUpdate(sql: string, params: SQLiteValue[]): ParsedUpdate {
  const match = sql.match(
    /UPDATE\s+["']?(\w+)["']?\s+SET\s+(.+?)(?:\s+WHERE\s+(.+))?$/i,
  )
  if (!match) throw new ERR_SQLITE_ERROR(`Invalid UPDATE: ${sql}`)

  const tableName = match[1]
  const setClause = match[2]
  const whereClause = match[3]

  const set: Record<string, SQLiteValue> = {}
  let paramIndex = 0
  const setParts = setClause.split(',').map((s) => s.trim())

  for (const part of setParts) {
    const [col, val] = part.split('=').map((s) => s.trim())
    const colName = col.replace(/["'`]/g, '')
    if (val === '?') {
      set[colName] = params[paramIndex++]
    } else {
      set[colName] = parseLiteral(val)
    }
  }

  let where: { column: string; value: SQLiteValue } | undefined
  if (whereClause) {
    const whereMatch = whereClause.match(/["']?(\w+)["']?\s*=\s*(.+)/i)
    if (whereMatch) {
      const value = whereMatch[2].trim()
      where = {
        column: whereMatch[1],
        value: value === '?' ? params[paramIndex] : parseLiteral(value),
      }
    }
  }

  return { type: 'UPDATE', tableName, set, where }
}

function parseDelete(sql: string, params: SQLiteValue[]): ParsedDelete {
  const match = sql.match(
    /DELETE\s+FROM\s+["']?(\w+)["']?(?:\s+WHERE\s+(.+))?$/i,
  )
  if (!match) throw new ERR_SQLITE_ERROR(`Invalid DELETE: ${sql}`)

  const tableName = match[1]
  let where: { column: string; value: SQLiteValue } | undefined

  if (match[2]) {
    const whereMatch = match[2].match(/["']?(\w+)["']?\s*=\s*(.+)/i)
    if (whereMatch) {
      const value = whereMatch[2].trim()
      where = {
        column: whereMatch[1],
        value: value === '?' ? params[0] : parseLiteral(value),
      }
    }
  }

  return { type: 'DELETE', tableName, where }
}

function parsePragma(sql: string): ParsedPragma {
  const match = sql.match(/PRAGMA\s+(\w+)(?:\s*=\s*(.+))?/i)
  if (!match) throw new ERR_SQLITE_ERROR(`Invalid PRAGMA: ${sql}`)
  return { type: 'PRAGMA', name: match[1], value: match[2] }
}

function parseLiteral(value: string): SQLiteValue {
  if (value === 'NULL' || value === 'null') return null
  if (value === 'TRUE' || value === 'true') return 1
  if (value === 'FALSE' || value === 'false') return 0
  if (
    (value.startsWith("'") && value.endsWith("'")) ||
    (value.startsWith('"') && value.endsWith('"'))
  ) {
    return value.slice(1, -1)
  }
  const num = Number(value)
  if (!Number.isNaN(num)) return num
  return value
}

// =============================================================================
// Database Class
// =============================================================================

export class Database {
  private readonly filename: string
  private readonly isSQLit: boolean
  private readonly isReadonly: boolean
  private readonly sqlitClient: SQLitHttpClient | null
  private readonly storage: InMemoryStorage
  private statements = new Map<string, Statement<SQLiteRow>>()
  private _inTransaction = false
  private _open = true

  constructor(filename: string = ':memory:', options?: SQLiteDatabaseOptions) {
    this.filename = filename
    this.isReadonly = options?.readonly ?? false

    // Check if connecting to SQLit
    this.isSQLit =
      filename.startsWith('sqlit://') || filename.startsWith('http')

    if (this.isSQLit) {
      // Parse SQLit connection string
      let config: SQLitConfig

      if (filename.startsWith('sqlit://')) {
        // Parse sqlit://dbid or sqlit://dbid?endpoint=http://...
        const urlPart = filename.slice(8) // Remove 'sqlit://'
        const [dbidPart, queryString] = urlPart.split('?')
        const params = new URLSearchParams(queryString ?? '')
        const endpointOverride = params.get('endpoint')

        config = {
          ...getSQLitConfigFromEnv(),
          dbid: dbidPart,
          ...(endpointOverride && { endpoint: endpointOverride }),
        }
      } else {
        // Direct HTTP URL
        const url = new URL(filename)
        config = {
          endpoint: `${url.protocol}//${url.host}`,
          dbid: url.pathname.slice(1) || 'default',
          timeout: 30000,
          debug: false,
        }
      }

      this.sqlitClient = new SQLitHttpClient(config)
      this.storage = new InMemoryStorage() // Not used for SQLit
    } else if (filename === ':memory:') {
      this.sqlitClient = null
      this.storage = new InMemoryStorage()
    } else {
      // File-based SQLite is not available in workerd - fail fast
      throw new ERR_SQLITE_ERROR(
        `File-based SQLite (${filename}) is not available in workerd. Use ':memory:' for in-memory database or 'sqlit://<database-id>' to connect to SQLit.`,
      )
    }
  }

  /**
   * Execute raw SQL statements
   */
  exec(sql: string): void {
    if (!this._open) throw new ERR_SQLITE_ERROR('Database is closed')

    const statements = sql.split(';').filter((s) => s.trim())
    for (const stmt of statements) {
      this.executeSync(stmt.trim(), [])
    }
  }

  /**
   * Run a SQL query with parameters
   */
  run(sql: string, ...params: SQLiteValue[]): SQLiteRunResult {
    if (!this._open) throw new ERR_SQLITE_ERROR('Database is closed')
    return this.executeSync(sql, params)
  }

  /**
   * Prepare a statement for execution
   */
  prepare<T = SQLiteRow>(sql: string): Statement<T> {
    if (!this._open) throw new ERR_SQLITE_ERROR('Database is closed')

    const cached = this.statements.get(sql)
    if (cached) {
      return cached as unknown as Statement<T>
    }

    const stmt = new Statement<T>(this, sql)
    this.statements.set(sql, stmt as unknown as Statement<SQLiteRow>)
    return stmt
  }

  /**
   * Execute a query and get all results
   */
  query<T = SQLiteRow>(sql: string, ...params: SQLiteValue[]): T[] {
    if (!this._open) throw new ERR_SQLITE_ERROR('Database is closed')
    const stmt = this.prepare<T>(sql)
    return stmt.all(...params)
  }

  /**
   * Begin a transaction
   */
  transaction<T>(fn: SQLiteTransactionFunction<T>): T {
    if (!this._open) throw new ERR_SQLITE_ERROR('Database is closed')

    this._inTransaction = true
    try {
      const result = fn(this)
      return result
    } finally {
      this._inTransaction = false
    }
  }

  /**
   * Close the database
   */
  close(): void {
    this._open = false
    this.statements.clear()
    this.storage.clear()
  }

  get path(): string {
    return this.filename
  }

  get open(): boolean {
    return this._open
  }

  get inMemory(): boolean {
    return this.filename === ':memory:'
  }

  get inTransaction(): boolean {
    return this._inTransaction
  }

  // Internal execution method
  executeSync(sql: string, params: SQLiteValue[]): SQLiteRunResult {
    if (this.isSQLit && this.sqlitClient) {
      // For SQLit, we need to use async but wrap it for sync interface
      // This is a limitation - real async should be used in production
      throw new ERR_SQLITE_ERROR(
        'SQLit backend requires async execution. Use Database.queryAsync() or Database.execAsync() instead.',
      )
    }

    // In-memory execution
    const parsed = parseSQL(sql, params)
    let changes = 0
    let lastInsertRowid: number | bigint = 0

    switch (parsed.type) {
      case 'CREATE_TABLE':
        this.storage.createTable(parsed.tableName, parsed.columns)
        break

      case 'DROP_TABLE':
        this.storage.dropTable(parsed.tableName)
        break

      case 'INSERT': {
        const result = this.storage.insert(
          parsed.tableName,
          parsed.columns,
          parsed.values,
        )
        changes = 1
        lastInsertRowid = result.lastInsertRowid
        break
      }

      case 'UPDATE':
        changes = this.storage.update(
          parsed.tableName,
          parsed.set,
          parsed.where,
        )
        break

      case 'DELETE':
        changes = this.storage.delete(parsed.tableName, parsed.where)
        break

      case 'SELECT':
      case 'PRAGMA':
        // No changes for SELECT/PRAGMA
        break
    }

    return { changes, lastInsertRowid }
  }

  // Internal select method for statements
  selectSync(sql: string, params: SQLiteValue[]): SQLiteRow[] {
    if (this.isSQLit) {
      throw new ERR_SQLITE_ERROR(
        'SQLit backend requires async execution. Use Database.queryAsync() instead.',
      )
    }

    const parsed = parseSQL(sql, params)
    if (parsed.type !== 'SELECT') {
      return []
    }

    return this.storage.select(
      parsed.tableName,
      parsed.columns,
      parsed.where,
      parsed.orderBy,
      parsed.limit,
      parsed.offset,
      parsed.aggregates,
    )
  }

  // Async methods for SQLit backend
  async queryAsync<T = SQLiteRow>(
    sql: string,
    params: SQLiteValue[] = [],
  ): Promise<T[]> {
    if (!this._open) throw new ERR_SQLITE_ERROR('Database is closed')

    if (this.isSQLit && this.sqlitClient) {
      const rows = await this.sqlitClient.query(sql, params)
      return rows as T[]
    }

    // Fall back to sync for in-memory
    return this.selectSync(sql, params) as T[]
  }

  async execAsync(
    sql: string,
    params: SQLiteValue[] = [],
  ): Promise<SQLiteRunResult> {
    if (!this._open) throw new ERR_SQLITE_ERROR('Database is closed')

    if (this.isSQLit && this.sqlitClient) {
      const result = await this.sqlitClient.exec(sql, params)
      return {
        changes: result.rowsAffected,
        lastInsertRowid: result.lastInsertId,
      }
    }

    // Fall back to sync for in-memory
    return this.executeSync(sql, params)
  }
}

// =============================================================================
// Statement Class
// =============================================================================

export class Statement<T = SQLiteRow> implements SQLiteStatement<T> {
  private readonly db: Database
  private readonly sql: string
  private readonly _columnNames: string[] = []
  private readonly _paramsCount: number
  private finalized = false

  constructor(db: Database, sql: string) {
    this.db = db
    this.sql = sql
    this._paramsCount = (sql.match(/\?/g) ?? []).length
  }

  run(...params: SQLiteValue[]): SQLiteRunResult {
    if (this.finalized) throw new ERR_SQLITE_ERROR('Statement has been finalized')
    return this.db.executeSync(this.sql, params)
  }

  all(...params: SQLiteValue[]): T[] {
    if (this.finalized) throw new ERR_SQLITE_ERROR('Statement has been finalized')
    return this.db.selectSync(this.sql, params) as T[]
  }

  get(...params: SQLiteValue[]): T | null {
    const rows = this.all(...params)
    return rows[0] ?? null
  }

  values(...params: SQLiteValue[]): SQLiteValue[][] {
    const rows = this.all(...params)
    return rows.map((row) => Object.values(row as SQLiteRow))
  }

  finalize(): void {
    this.finalized = true
  }

  get columnNames(): string[] {
    return this._columnNames
  }

  get paramsCount(): number {
    return this._paramsCount
  }
}

// =============================================================================
// Constants
// =============================================================================

export const SQLITE_VERSION = '3.45.0'
export const SQLITE_VERSION_NUMBER = 3045000

export const OPEN_READONLY = 1
export const OPEN_READWRITE = 2
export const OPEN_CREATE = 4

export const SQLITE_OK = 0
export const SQLITE_ERROR = 1
export const SQLITE_BUSY = 5
export const SQLITE_LOCKED = 6
export const SQLITE_NOTFOUND = 12
export const SQLITE_MISUSE = 21

// =============================================================================
// Default Export
// =============================================================================

export default {
  Database,
  Statement,
  SQLITE_VERSION,
  SQLITE_VERSION_NUMBER,
  OPEN_READONLY,
  OPEN_READWRITE,
  OPEN_CREATE,
  SQLITE_OK,
  SQLITE_ERROR,
  SQLITE_BUSY,
  SQLITE_LOCKED,
  SQLITE_NOTFOUND,
  SQLITE_MISUSE,
}
