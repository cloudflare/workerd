import { DurableObject } from "cloudflare:workers";

export class Counter extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec("CREATE TABLE IF NOT EXISTS hits (n INTEGER)");
  }
  async increment() {
    this.sql.exec("INSERT INTO hits VALUES (1)");
    return this.sql.exec("SELECT COUNT(*) AS n FROM hits").one().n;
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/self") {
      const r = await env.SELF.fetch("http://x/");
      return new Response("via service binding: " + (await r.text()));
    }
    const id = env.COUNTER.idFromName(url.pathname);
    const n = await env.COUNTER.get(id).increment();
    return new Response(`${env.GREETING} (max ${env.LIMITS.max}): ${url.pathname} hit ${n} times\n`);
  }
};
