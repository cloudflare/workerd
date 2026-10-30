// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { DurableObject } from 'cloudflare:workers';

interface Env {
  GREETING: string;
}

// Types `ctx.exports`: the main module's exports, and which are Durable Objects.
declare global {
  namespace Cloudflare {
    interface GlobalProps {
      mainModule: typeof import('./index');
      durableNamespaces: 'Counter';
    }
  }
}

// One counter per name, stored in the object's SQLite database.
export class Counter extends DurableObject<Env> {
  increment(): number {
    const sql = this.ctx.storage.sql;
    sql.exec('CREATE TABLE IF NOT EXISTS n (v INTEGER)');
    const v = (sql.exec('SELECT v FROM n').toArray()[0]?.v as number) ?? 0;
    sql.exec('DELETE FROM n');
    sql.exec('INSERT INTO n VALUES (?)', v + 1);
    return v + 1;
  }
}

export default {
  async fetch(request: Request, env: Env, ctx): Promise<Response> {
    const name = new URL(request.url).pathname.slice(1) || 'world';
    const n = await ctx.exports.Counter.getByName(name).increment();
    return new Response(`${env.GREETING}, ${name}! (visit ${n})\n`);
  },
} satisfies ExportedHandler<Env>;
