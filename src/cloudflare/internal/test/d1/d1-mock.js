// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export class D1MockDO {
  constructor(state, env) {
    this.state = state
    this.sql = this.state.storage.sql
  }

  async fetch(request) {
    const { pathname, searchParams } = new URL(request.url)
    const is_query = pathname === '/query'
    const is_execute = pathname === '/execute'
    if (request.method === 'POST' && (is_query || is_execute)) {
      const body = await request.json()
      const resultsFormat =
        searchParams.get('resultsFormat') ??
        (is_query ? 'ARRAY_OF_OBJECTS' : 'NONE')
      return Response.json(
        Array.isArray(body)
          ? body.map((query) => this.runQuery(query, resultsFormat))
          : this.runQuery(body, resultsFormat)
      )
    } else {
      return Response.json({ error: 'Not found' }, { status: 404 })
    }
  }

  runQuery(query, resultsFormat) {
    const { sql, params = [] } = query
    const stmt = this.sql.prepare(sql)(...params)
    const columnNames = stmt.columnNames
    const rawResults = Array.from(stmt.raw())

    const results =
      resultsFormat === 'NONE'
        ? undefined
        : resultsFormat === 'ROWS_AND_COLUMNS'
        ? { columns: columnNames, rows: rawResults }
        : rawResults.map((row) =>
            Object.fromEntries(columnNames.map((c, i) => [c, row[i]]))
          )

    return {
      success: true,
      results,
      meta: { duration: 0.001, served_by: 'd1-mock' },
    }
  }
}

export default {
  async fetch(request, env, ctx) {
    try {
      const stub = env.db.get(env.db.idFromName('test'))
      return stub.fetch(request)
    } catch (err) {
      return Response.json(
        { error: err.message, stack: err.stack },
        { status: 500 }
      )
    }
  },
}
