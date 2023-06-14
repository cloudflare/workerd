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
      const body = await request.json()

      if (request.method === 'POST' && pathname.startsWith('/query')) {
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
