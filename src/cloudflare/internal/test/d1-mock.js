export default {
  async fetch(request, env, ctx) {
    try {
      const { pathname } = new URL(request.url)
      const body = await request.json()

      if (request.method === 'POST' && pathname.startsWith('/query')) {
        const { sql, params } = body
        switch (sql) {
          case 'select 1':
            return ok({ 1: 1 })
          default:
            return Response.json({ error: 'Unmocked query' }, { status: 400 })
        }
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
  return Response.json({
    results,
    meta: { duration: 0.001, served_by: 'd1-mock' },
  })
}
