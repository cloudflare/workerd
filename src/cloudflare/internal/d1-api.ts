// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

interface Fetcher {
  fetch: typeof fetch
}

interface D1Result<T = unknown> {
  results: T[]
  success: true
  meta: Record<string, unknown>
  error?: never
}

interface D1UpstreamFailure {
  results?: never
  error: string
  success: false
  meta: Record<string, unknown>
}

type D1UpstreamResponse<T = unknown> = D1Result<T> | D1UpstreamFailure

interface D1ExecResult {
  count: number
  duration: number
}

interface SQLError {
  error: string
}

class D1Database {
  private readonly fetcher: Fetcher

  public constructor(fetcher: Fetcher) {
    this.fetcher = fetcher
  }

  public prepare(query: string): D1PreparedStatement {
    return new D1PreparedStatement(this, query)
  }

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
    const exec = await this._sendOrThrow(
      '/query',
      statements.map((s: D1PreparedStatement) => s.statement),
      statements.map((s: D1PreparedStatement) => s.params)
    )
    return exec as D1Result<T>[]
  }

  public async exec(query: string): Promise<D1ExecResult> {
    // should be /execute - see CFSQL-52
    const lines = query.trim().split('\n')
    const _exec = await this._send('/query', lines, [])
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
    query: unknown,
    params: unknown[]
  ): Promise<D1Result<T>[] | D1Result<T>> {
    const results = await this._send(endpoint, query, params)
    const firstResult = firstIfArray(results)
    if (!firstResult.success) {
      throw new Error(`D1_ERROR: ${firstResult.error}`, {
        cause: new Error(firstResult.error),
      })
    } else {
      return results as D1Result<T>[] | D1Result<T>
    }
  }

  public async _send<T = unknown>(
    endpoint: string,
    query: unknown,
    params: unknown[]
  ): Promise<D1UpstreamResponse<T>[] | D1UpstreamResponse<T>> {
    /* this needs work - we currently only support ordered ?n params */
    const body = JSON.stringify(
      typeof query == 'object'
        ? (query as string[]).map((s: string, index: number) => {
            return { sql: s, params: params[index] }
          })
        : {
            sql: query,
            params: params,
          }
    )

    const response = await this.fetcher.fetch(new URL(endpoint, 'http://d1'), {
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
        this.params
      )
    )

    const results = info.results
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

  public async run<T = Record<string, unknown>>(): Promise<D1Result<T>> {
    return firstIfArray(
      await this.database._sendOrThrow<T>(
        '/execute',
        this.statement,
        this.params
      )
    )
  }

  public async all<T = Record<string, unknown>>(): Promise<D1Result<T[]>> {
    return firstIfArray(
      await this.database._sendOrThrow<T[]>(
        '/query',
        this.statement,
        this.params
      )
    )
  }

  public async raw<T = unknown[]>(): Promise<T[]> {
    const s = firstIfArray(
      await this.database._sendOrThrow<Record<string, unknown>>(
        '/query',
        this.statement,
        this.params
      )
    )
    const raw: T[] = []
    for (const row of s.results) {
      const entry = Object.keys(row).map((k) => {
        return row[k]
      })
      raw.push(entry as T)
    }
    return raw
  }
}

function firstIfArray<T>(results: T | T[]): T {
  return Array.isArray(results) ? results[0]! : results
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
        results: result.results || [],
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
