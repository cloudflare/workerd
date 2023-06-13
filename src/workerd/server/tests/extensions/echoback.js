export default {
  async fetch(request, env, ctx) {
    try {
      const { url, method } = request
      const headers = {}

      for (const [k, v] of request.headers.entries()) {
        headers[k] = v
      }

      const payload = {
        headers,
        url,
        method,
        env: Object.keys(env),
        body: await request.text(),
      }
      for (const k of Object.entries(payload)) {
        console.log(...k)
      }
      return Response.json(payload, {
        headers: {
          'content-type': 'application/json',
        },
      })
    } catch (err) {
      Response.json({ error: err.message, stack: err.stack }, { status: 500 })
    }
  },
}
