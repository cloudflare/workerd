export function wrap(env) {
  return async (json) => {
    const request = new Request("http://fake-host", { method: "POST", body: JSON.stringify(json) } );
    return await (await env.target.fetch(request)).json();
  }
}

export function unwrap(target) {
  const { fetchJson, ...rest } = target;
  return {
    ...rest,
    async fetch(req, env) {
      const json = await req.json();
      const reply = await target.fetchJson(json, env);
      return new Response(JSON.stringify(reply));
    }
  };
}
