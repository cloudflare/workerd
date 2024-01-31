// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

interface Fetcher {
  fetch: typeof fetch
}

type D1Response = {
  success: true
  meta: Record<string, unknown>
  error?: never
}

type D1Result<T = unknown> = D1Response & {
  results: T[]
}

type D1RawOptions = {
  columnNames?: boolean
}

type D1UpstreamFailure = {
  results?: never
  error: string
  success: false
  meta: Record<string, unknown>
}

type D1RowsColumns<T = unknown> = D1Response & {
  results: {
    columns: string[]
    rows: T[][]
  }
}

type D1UpstreamSuccess<T = unknown> =
  | D1Result<T>
  | D1Response
  | D1RowsColumns<T>

type D1UpstreamResponse<T = unknown> = D1UpstreamSuccess<T> | D1UpstreamFailure

type D1ExecResult = {
  count: number
  duration: number
}

type SQLError = {
  error: string
}

type ResultsFormat = 'ARRAY_OF_OBJECTS' | 'ROWS_AND_COLUMNS' | 'NONE'

class D1Database {
  private readonly fetcher: Fetcher

  public constructor(fetcher: Fetcher) {
    this.fetcher = fetcher
  }

  public prepare(query: string): D1PreparedStatement {
    return new D1PreparedStatement(this, query)
  }

  // DEPRECATED, TO BE REMOVED WITH NEXT BREAKING CHANGE
  public async dump(): Promise<ArrayBuffer> {
    const response = await this.fetcher.fetch('http://d1/dump', {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
      },
    })
    if (response.status !== 200) {
      try {
        const err = (await response.json()) as SQLError
        throw new Error(`D1_DUMP_ERROR: ${err.error}`, {
          cause: new Error(err.error),
        })
      } catch (e) {
        throw new Error(`D1_DUMP_ERROR: Status + ${response.status}`, {
          cause: new Error(`Status ${response.status}`),
        })
      }
    }
    return await response.arrayBuffer()
  }

  public async batch<T = unknown>(
    statements: D1PreparedStatement[]
  ): Promise<D1Result<T>[]> {
    const exec = (await this._sendOrThrow(
      '/query',
      statements.map((s: D1PreparedStatement) => s.statement),
      statements.map((s: D1PreparedStatement) => s.params),
      'ROWS_AND_COLUMNS'
    )) as D1UpstreamSuccess<T>[]
    return exec.map(toArrayOfObjects)
  }

  public async exec(query: string): Promise<D1ExecResult> {
    const lines = query.trim().split('\n')
    const _exec = await this._send('/execute', lines, [], 'NONE')
    const exec = Array.isArray(_exec) ? _exec : [_exec]
    const error = exec
      .map((r) => {
        return r.error ? 1 : 0
      })
      .indexOf(1)
    if (error !== -1) {
      throw new Error(
        `D1_EXEC_ERROR: Error in line ${error + 1}: ${lines[error]}: ${
          exec[error]?.error
        }`,
        {
          cause: new Error(
            `Error in line ${error + 1}: ${lines[error]}: ${exec[error]?.error}`
          ),
        }
      )
    } else {
      return {
        count: exec.length,
        duration: exec.reduce((p, c) => {
          return p + (c.meta['duration'] as number)
        }, 0),
      }
    }
  }

  public async _sendOrThrow<T = unknown>(
    endpoint: string,
    query: string | string[],
    params: unknown[],
    resultsFormat: ResultsFormat
  ): Promise<D1UpstreamSuccess<T>[] | D1UpstreamSuccess<T>> {
    const results = await this._send(endpoint, query, params, resultsFormat)
    const firstResult = firstIfArray(results)
    if (!firstResult.success) {
      throw new Error(`D1_ERROR: ${firstResult.error}`, {
        cause: new Error(firstResult.error),
      })
    } else {
      return results as D1UpstreamSuccess<T>[] | D1UpstreamSuccess<T>
    }
  }

  public async _send<T = unknown>(
    endpoint: string,
    query: string | string[],
    params: unknown[],
    resultsFormat: ResultsFormat
  ): Promise<D1UpstreamResponse<T>[] | D1UpstreamResponse<T>> {
    /* this needs work - we currently only support ordered ?n params */
    const body = JSON.stringify(
      Array.isArray(query)
        ? query.map((s: string, index: number) => {
            return { sql: s, params: params[index] }
          })
        : {
            sql: query,
            params: params,
          }
    )

    const url = new URL(endpoint, 'http://d1')
    url.searchParams.set('resultsFormat', resultsFormat)
    const response = await this.fetcher.fetch(url.href, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
      },
      body,
    })

    try {
      const answer = await toJson<
        D1UpstreamResponse<T>[] | D1UpstreamResponse<T>
      >(response)

      if (Array.isArray(answer)) {
        return answer.map((r: D1UpstreamResponse<T>) => mapD1Result<T>(r))
      } else {
        return mapD1Result<T>(answer)
      }
    } catch (_e: unknown) {
      const e = _e as Error
      const message =
        (e.cause as Error | undefined)?.message ||
        e.message ||
        'Something went wrong'
      throw new Error(`D1_ERROR: ${message}`, {
        cause: new Error(message),
      })
    }
  }
}

class D1PreparedStatement {
  private readonly database: D1Database
  public readonly statement: string
  public readonly params: unknown[]

  public constructor(
    database: D1Database,
    statement: string,
    values?: unknown[]
  ) {
    this.database = database
    this.statement = statement
    this.params = values || []
  }

  public bind(...values: unknown[]): D1PreparedStatement {
    // Validate value types
    const transformedValues = values.map((r: unknown): unknown => {
      if (typeof r === 'number' || typeof r === 'string') {
        return r
      } else if (typeof r === 'object') {
        // nulls are objects in javascript
        if (r == null) return r
        // arrays with uint8's are good
        if (
          Array.isArray(r) &&
          r.every((b: unknown) => {
            return typeof b == 'number' && b >= 0 && b < 256
          })
        )
          return r as unknown[]
        // convert ArrayBuffer to array
        if (r instanceof ArrayBuffer) {
          return Array.from(new Uint8Array(r))
        }
        // convert view to array
        if (ArrayBuffer.isView(r)) {
          // For some reason TS doesn't think this is valid, but it is!
          return Array.from(r as unknown as ArrayLike<unknown>)
        }
      }

      throw new Error(
        `D1_TYPE_ERROR: Type '${typeof r}' not supported for value '${r}'`,
        {
          cause: new Error(`Type '${typeof r}' not supported for value '${r}'`),
        }
      )
    })
    return new D1PreparedStatement(
      this.database,
      this.statement,
      transformedValues
    )
  }

  public async first<T = unknown>(colName: string): Promise<T | null>
  public async first<T = Record<string, unknown>>(): Promise<T | null>
  public async first<T = unknown>(
    colName?: string
  ): Promise<Record<string, T> | T | null> {
    const info = firstIfArray(
      await this.database._sendOrThrow<Record<string, T>>(
        '/query',
        this.statement,
        this.params,
        'ROWS_AND_COLUMNS'
      )
    )

    const results = toArrayOfObjects(info).results
    const hasResults = results.length > 0
    if (!hasResults) return null

    const firstResult = results[0]!
    if (colName !== undefined) {
      if (hasResults && firstResult[colName] === undefined) {
        throw new Error(`D1_COLUMN_NOTFOUND: Column not found (${colName})`, {
          cause: new Error('Column not found'),
        })
      }
      return firstResult[colName]!
    } else {
      return firstResult
    }
  }

  public async run<T = Record<string, unknown>>(): Promise<D1Response> {
    return firstIfArray(
      await this.database._sendOrThrow<T>(
        '/execute',
        this.statement,
        this.params,
        'NONE'
      )
    )
  }

  public async all<T = Record<string, unknown>>(): Promise<D1Result<T[]>> {
    return toArrayOfObjects(
      firstIfArray(
        await this.database._sendOrThrow<T[]>(
          '/query',
          this.statement,
          this.params,
          'ROWS_AND_COLUMNS'
        )
      )
    )
  }

  public async raw<T = unknown[]>(options?: D1RawOptions): Promise<T[]> {
    const s = firstIfArray(
      await this.database._sendOrThrow<Record<string, unknown>>(
        '/query',
        this.statement,
        this.params,
        'ROWS_AND_COLUMNS'
      )
    )
    // If no results returned, return empty array
    if (!('results' in s)) return []

    // If ARRAY_OF_OBJECTS returned, extract cells
    if (Array.isArray(s.results)) {
      const raw: T[] = []
      for (const row of s.results) {
        if (options?.columnNames && raw.length === 0) {
          raw.push(Array.from(Object.keys(row)) as T)
        }
        const entry = Object.keys(row).map((k) => {
          return row[k]
        })
        raw.push(entry as T)
      }
      return raw
    } else {
      // Otherwise, data is already in the correct format
      return [
        ...(options?.columnNames ? [s.results.columns as T] : []),
        ...(s.results.rows as T[]),
      ]
    }
  }
}

function firstIfArray<T>(results: T | T[]): T {
  return Array.isArray(results) ? results[0]! : results
}

// This shim may be used against an older version of D1 that doesn't support
// the ROWS_AND_COLUMNS/NONE interchange format, so be permissive here
function toArrayOfObjects<T>(response: D1UpstreamSuccess<T>): D1Result<T> {
  // If 'results' is missing from upstream, add an empty array
  if (!('results' in response))
    return {
      ...response,
      results: [],
    }

  const results = response.results
  if (Array.isArray(results)) {
    return { ...response, results }
  } else {
    const { rows, columns } = results
    return {
      ...response,
      results: rows.map(
        (row) =>
          Object.fromEntries(row.map((cell, i) => [columns[i], cell])) as T
      ),
    }
  }
}

function mapD1Result<T>(result: D1UpstreamResponse<T>): D1UpstreamResponse<T> {
  // The rest of the app can guarantee that success is true/false, but from the API
  // we only guarantee that error is present/absent.
  return result.error
    ? {
        success: false,
        meta: result.meta || {},
        error: result.error,
      }
    : {
        success: true,
        meta: result.meta || {},
        ...('results' in result ? { results: result.results } : {}),
      }
}

async function toJson<T = unknown>(response: Response): Promise<T> {
  const body = await response.text()
  try {
    return JSON.parse(body) as T
  } catch (e) {
    throw new Error(`Failed to parse body as JSON, got: ${body}`)
  }
}

export default function makeBinding(env: { fetcher: Fetcher }): D1Database {
  return new D1Database(env.fetcher)
}
