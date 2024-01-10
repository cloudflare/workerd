// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export class D1MockDO {
  constructor(state, env) {
    this.state = state
    this.sql = this.state.storage.sql
    this.sql.exec(`
      CREATE TABLE IF NOT EXISTS users (
          user_id INTEGER PRIMARY KEY,
          name TEXT,
          home TEXT,
          features TEXT
      );`)
    this.sql.exec(`DELETE FROM users;`)
    this.sql.exec(`INSERT INTO users (name, home, features) VALUES 
      ('Albert Ross', 'sky', 'wingspan'),
      ('Al Dente', 'bowl', 'mouthfeel');
    `)
  }

  async fetch(request) {
    const { pathname } = new URL(request.url)
    if (request.method === 'POST' && pathname.match(/^\/(query|execute)$/)) {
      const body = await request.json()
      return Response.json(
        Array.isArray(body)
          ? body.map((query) => this.runQuery(query))
          : this.runQuery(body)
      )
    } else {
      return Response.json({ error: 'Not found' }, { status: 404 })
    }
  }

  runQuery(query) {
    const { sql, params = [] } = query
    const results = Array.from(this.sql.exec(sql, ...params))
    return {
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
