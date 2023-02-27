export default {
  async fetch(req, env) {
    const reply = await env.server({ type: "ping" });
    return new Response(`${JSON.stringify(reply)}\n`);
  }
};
