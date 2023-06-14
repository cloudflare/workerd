type Fetcher = {
  fetch: typeof fetch
}

type D1Result<T = unknown> = {
  results: T[]
  success: true
  meta: any
  error?: never
}

type D1UpstreamFailure = {
  results?: never
  error: string
  success: false
  meta: any
}
export type D1UpstreamResponse<T = unknown> = D1Result<T> | D1UpstreamFailure

export type D1ExecResult = {
  count: number
  duration: number
}

type SQLError = {
  error: string
}

export class D1Database {
  private readonly fetcher: Fetcher

  constructor(fetcher: Fetcher) {
    this.fetcher = fetcher
  }

  prepare(query: string): D1PreparedStatement {
    return new D1PreparedStatement(this, query)
  }

  async dump(): Promise<ArrayBuffer> {
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
          cause: new Error('Status ' + response.status),
        })
      }
    }
    return await response.arrayBuffer()
  }

  async batch<T = unknown>(
    statements: D1PreparedStatement[]
  ): Promise<D1Result<T>[]> {
    const exec = await this._sendOrThrow(
      '/query',
      statements.map((s: D1PreparedStatement) => s.statement),
      statements.map((s: D1PreparedStatement) => s.params)
    )
    return exec as D1Result<T>[]
  }

  async exec<T = unknown>(query: string): Promise<D1ExecResult> {
    // should be /execute - see CFSQL-52
    const lines = query.trim().split('\n')
    const _exec = await this._send<T>('/query', lines, [])
    const exec = Array.isArray(_exec) ? _exec : [_exec]
    const error = exec
      .map((r) => {
        return r.error ? 1 : 0
      })
      .indexOf(1)
    if (error !== -1) {
      throw new Error(
        `D1_EXEC_ERROR: Error in line ${error + 1}: ${lines[error]}: ${
          exec[error]!.error
        }`,
        {
          cause: new Error(
            'Error in line ' +
              (error + 1) +
              ': ' +
              lines[error] +
              ': ' +
              exec[error]!.error
          ),
        }
      )
    } else {
      return {
        count: exec.length,
        duration: exec.reduce((p, c) => {
          return p + c.meta.duration
        }, 0),
      }
    }
  }

  async _sendOrThrow<T = unknown>(
    endpoint: string,
    query: any,
    params: any[]
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

  async _send<T = unknown>(
    endpoint: string,
    query: any,
    params: any[]
  ): Promise<D1UpstreamResponse<T>[] | D1UpstreamResponse<T>> {
    /* this needs work - we currently only support ordered ?n params */
    const body = JSON.stringify(
      typeof query == 'object'
        ? (query as any[]).map((s: string, index: number) => {
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
      const answer = (await toJson(response)) as any[] | any

      if (Array.isArray(answer)) {
        return answer.map((r: any) => mapD1Result<T>(r))
      } else {
        return mapD1Result<T>(answer)
      }
    } catch (e: any) {
      const message = e.cause?.message || e.message || 'Something went wrong'
      throw new Error(`D1_ERROR: ${message}`, {
        cause: new Error(message),
      })
    }
  }
}

export class D1PreparedStatement {
  readonly statement: string
  private readonly database: D1Database
  params: any[]

  constructor(database: D1Database, statement: string, values?: any) {
    this.database = database
    this.statement = statement
    this.params = values || []
  }

  bind(...values: any[]) {
    // Validate value types
    for (var r in values) {
      switch (typeof values[r]) {
        case 'number':
        case 'string':
          break
        case 'object':
          // nulls are objects in javascript
          if (values[r] == null) break
          // arrays with uint8's are good
          if (
            Array.isArray(values[r]) &&
            values[r]
              .map((b: any) => {
                return typeof b == 'number' && b >= 0 && b < 256 ? 1 : 0
              })
              .indexOf(0) == -1
          )
            break
          // convert ArrayBuffer to array
          if (values[r] instanceof ArrayBuffer) {
            values[r] = Array.from(new Uint8Array(values[r]))
            break
          }
          // convert view to array
          if (ArrayBuffer.isView(values[r])) {
            values[r] = Array.from(values[r])
            break
          }
          // Unreachable, added for compiler's sake
          break
        default:
          throw new Error(
            `D1_TYPE_ERROR: Type '${typeof values[
              r
            ]}' not supported for value '${values[r]}'`,
            {
              cause: new Error(
                "Type '" +
                  typeof values[r] +
                  "' not supported for value '" +
                  values[r] +
                  "'"
              ),
            }
          )
      }
    }
    return new D1PreparedStatement(this.database, this.statement, values)
  }

  async first<T = unknown>(colName: string): Promise<T | null>
  async first<T = unknown>(
    colName: undefined
  ): Promise<Record<string, T> | null>
  async first<T = unknown>(
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

  async run<T = unknown>(): Promise<D1Result<T>> {
    return firstIfArray(
      await this.database._sendOrThrow<T>(
        '/execute',
        this.statement,
        this.params
      )
    )
  }

  async all<T = unknown>(): Promise<D1Result<T[]>> {
    return firstIfArray(
      await this.database._sendOrThrow<T[]>(
        '/query',
        this.statement,
        this.params
      )
    )
  }

  async raw<T = unknown>(): Promise<T[]> {
    const s = firstIfArray(
      await this.database._sendOrThrow<any>(
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

async function toJson(response: Response) {
  const body = await response.text()
  try {
    return JSON.parse(body)
  } catch (e) {
    throw new Error(`Failed to parse body as JSON, got: ${body}`)
  }
}

export default function makeBinding(env: { fetcher: Fetcher }): D1Database {
  console.log(env)
  return new D1Database(env.fetcher)
}
