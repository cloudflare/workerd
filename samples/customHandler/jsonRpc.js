export function wrap(target) {
  return async (json) => {
    const request = new Request("http://fake-host", { method: "POST", body: JSON.stringify(json) } );
    const response = await target.fetch(request);
    return await response.json();
  }
}

export function unwrap(target) {
  return {
    async fetch(req, env) {
      const json = await req.json();
      const reply = await target.fetchJson(json, env);
      return new Response(JSON.stringify(reply));
    }
  };
}
