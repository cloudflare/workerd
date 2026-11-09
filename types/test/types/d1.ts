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
      expectType<D1DatabaseSession>(env.DB.withSession());
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

// CUSTOM BINDING
export class D1CustomBinding implements D1BindingService {
  constructor(private readonly db: D1Database & D1BindingService) {}

  async query(params: D1QueryRequest): Promise<D1QueryResponse> {
    for (const stmt of params.queries) {
      expectType<string>(stmt.sql);
      expectType<unknown[] | undefined>(stmt.params);
    }
    expectType<string | undefined>(params.bookmark);

    const response = await this.db.query(params);
    expectType<D1SessionBookmark | undefined>(response.bookmark);
    const [result] = response.results;
    expectType<D1Meta>(result.meta);
    expectType<'raw'>(result.data.kind);
    expectType<string[]>(result.data.columns);
    expectType<unknown[][]>(result.data.rows);
    return response;
  }
}

{
  const constraint: D1QueryRequest = { queries: [], bookmark: 'first-primary' };
  const bookmark: D1QueryRequest = { queries: [], bookmark: 'any-bookmark' };
  expectType<D1QueryRequest[]>([constraint, bookmark]);
}

// query() is not yet part of the public D1Database API. The method will be
// available once the `d1_binding_jsrpc` compatibility flag is removed and all
// D1 bindings use the JSRPC binding.
// @ts-expect-error
export const noQuery: D1Database['query'] = undefined;
