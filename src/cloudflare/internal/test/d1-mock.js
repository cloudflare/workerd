// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

const MOCK_USER_ROWS = {
  1: { user_id: 1, name: 'Albert Ross', home: 'sky', features: 'wingspan' },
  2: { user_id: 2, name: 'Al Dente', home: 'bowl', features: 'mouthfeel' },
}

function mockQuery({ sql, params }) {
  switch (sql.trim()) {
    case 'select 1;':
      return ok({ 1: 1 })
    case 'select * from users;':
      return ok(...Object.values(MOCK_USER_ROWS))
    case 'select * from users where user_id = ?;':
      return ok(MOCK_USER_ROWS[params[0]])
    default:
      throw new Error(`Unmocked query: ${sql}`)
  }
}

export default {
  async fetch(request, env, ctx) {
    try {
      const { pathname } = new URL(request.url)

      if (request.method === 'POST' && pathname.startsWith('/query')) {
        const body = await request.json()
        return Response.json(
          Array.isArray(body)
            ? body.map((query) => mockQuery(query))
            : mockQuery(body)
        )
      } else {
        return Response.json({ error: 'Not found' }, { status: 404 })
      }
    } catch (err) {
      return Response.json(
        { error: err.message, stack: err.stack },
        { status: 500 }
      )
    }
  },
}

function ok(...results) {
  return {
    results,
    meta: { duration: 0.001, served_by: 'd1-mock' },
  }
}
