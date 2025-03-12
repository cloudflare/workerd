// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

type Row = { col1: string; col2: string };

export const handler: ExportedHandler<{ DB: D1Database }> = {
  async fetch(request, env) {
    const stmt = env.DB.prepare(`SELECT * FROM tbl WHERE id = ?`);

    // RUN
    {
      const response = await stmt.bind(1).run();
      expectType<{
        meta: Record<string, unknown>;
        success: boolean;
        results?: any;
      }>(response);
    }

    // ALL
    {
      const { results } = await stmt.bind(1).all();
      expectType<Record<string, unknown>[]>(results);
    }
    {
      const { results } = await stmt.bind(1).all<Row>();
      expectType<Row[]>(results);
    }

    // RAW
    {
      const results = await stmt.bind(1).raw();
      expectType<unknown[][]>(results);
    }
    {
      const results = await stmt.bind(1).raw<[string, number, boolean]>();
      expectType<[string, number, boolean][]>(results);
    }
    {
      const results = await stmt.bind(1).raw<[string, number, boolean]>({});
      expectType<[string, number, boolean][]>(results);
    }
    {
      const results = await stmt
        .bind(1)
        .raw<[string, number, boolean]>({ columnNames: false });
      expectType<[string, number, boolean][]>(results);
    }
    {
      const results = await stmt.bind(1).raw({ columnNames: true });
      expectType<[string[], ...unknown[]]>(results);
    }
    {
      const [columns, ...rows] = await stmt
        .bind(1)
        .raw<[string, number, boolean]>({ columnNames: true });
      expectType<string[]>(columns);
      expectType<[string, number, boolean][]>(rows);
    }

    // FIRST
    {
      const result = await stmt.bind(1).first();
      expectType<Record<string, unknown> | null>(result);
    }
    {
      const result = await stmt.bind(1).first<Row>();
      expectType<Row | null>(result);
    }

    // FIRST (col)
    {
      const result = await stmt.bind(1).first("col1");
      expectType<unknown | null>(result);
    }
    {
      const result = await stmt.bind(1).first<string>("col1");
      expectType<string | null>(result);
    }

    // BATCH
    {
      const results = await env.DB.batch([stmt.bind(1), stmt.bind(2)]);
      expectType<D1Result[]>(results);
      expectType<unknown[]>(results[0].results);
    }
    {
      const results = await env.DB.batch<Row>([stmt.bind(1), stmt.bind(2)]);
      expectType<D1Result<Row>[]>(results);
      expectType<Row[]>(results[0].results);
    }

    // EXEC
    {
      const response = await env.DB.exec(`
       select 1;
       select * from tbl;
     `);
      expectType<{ count: number; duration: number }>(response);
    }

    // WITHSESSION
    {
      const session = env.DB.withSession("first-primary");
      expectType<D1DatabaseSession>(session);

      const bookmark = session.getBookmark();
      expectType<D1SessionBookmark | null>(bookmark);

      // SESSION PREPARE
      const stmt = session.prepare(`SELECT * FROM tbl WHERE id = ?`);

      // SESSION BATCH
      {
        const results = await env.DB.batch([stmt.bind(1), stmt.bind(2)]);
        expectType<D1Result[]>(results);
        expectType<unknown[]>(results[0].results);
      }
      {
        const results = await env.DB.batch<Row>([stmt.bind(1), stmt.bind(2)]);
        expectType<D1Result<Row>[]>(results);
        expectType<Row[]>(results[0].results);
      }
    }

    return new Response();
  },
};
