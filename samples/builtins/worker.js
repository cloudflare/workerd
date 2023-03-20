import { open } from "alibaba:cave"

export default {
  async fetch(req, env) {
    const key = await req.text();
    return new Response(open(key));
  }
};
