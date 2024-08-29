// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export class D1MockDO {
  constructor(state, env) {
    this.state = state;
    this.sql = this.state.storage.sql;
  }

  async fetch(request) {
    const { pathname, searchParams } = new URL(request.url);
    const is_query = pathname === '/query';
    const is_execute = pathname === '/execute';
    if (request.method === 'POST' && (is_query || is_execute)) {
      const body = await request.json();
      const resultsFormat =
        searchParams.get('resultsFormat') ??
        (is_query ? 'ARRAY_OF_OBJECTS' : 'NONE');
      return Response.json(
        Array.isArray(body)
          ? body.map((query) => this.runQuery(query, resultsFormat))
          : this.runQuery(body, resultsFormat)
      );
    } else {
      return Response.json({ error: 'Not found' }, { status: 404 });
    }
  }

  runQuery(query, resultsFormat) {
    const { sql, params = [] } = query;

    const changes_stmt = this.sql.prepare(
      `SELECT total_changes() as changes, last_insert_rowid() as last_row_id`
    );
    const size_before = this.sql.databaseSize;
    const [[changes_before, last_row_id_before]] = Array.from(
      changes_stmt().raw()
    );

    const stmt = this.sql.prepare(sql)(...params);
    const columnNames = stmt.columnNames;
    const rawResults = Array.from(stmt.raw());

    const results =
      resultsFormat === 'NONE'
        ? undefined
        : resultsFormat === 'ROWS_AND_COLUMNS'
          ? { columns: columnNames, rows: rawResults }
          : rawResults.map((row) =>
              Object.fromEntries(columnNames.map((c, i) => [c, row[i]]))
            );

    const [[changes_after, last_row_id_after]] = Array.from(
      changes_stmt().raw()
    );

    const size_after = this.sql.databaseSize;
    const num_changes = changes_after - changes_before;
    const has_changes = num_changes !== 0;
    const last_row_changed = last_row_id_after !== last_row_id_before;

    const db_size_different = size_after != size_before;

    // `changed_db` includes multiple ways the DB might be altered
    const changed_db = has_changes || last_row_changed || db_size_different;

    const { rowsRead: rows_read, rowsWritten: rows_written } = stmt;

    return {
      success: true,
      results,
      meta: {
        duration: Math.random() * 0.01,
        served_by: 'd1-mock',
        changes: num_changes,
        last_row_id: last_row_id_after,
        changed_db,
        size_after,
        rows_read,
        rows_written,
      },
    };
  }
}

export default {
  commitTokenNum: 0,
  commitTokensReceived: [],
  commitTokensReturned: [],
  nextTokenExpected: null,

  async fetch(request, env, ctx) {
    if (request.url.startsWith('http://d1-api-test/commitTokens')) {
      return this.handleD1ApiTestRoutes(request);
    }

    // For our testing purposes, record any commit token passed through.
    const reqCommitToken = request.headers.get('x-cf-d1-session-commit-token');
    this.commitTokensReceived.push(reqCommitToken);

    try {
      const stub = env.db.get(env.db.idFromName('test'));

      // Add a commitToken to all responses.
      return stub
        .fetch(request)
        .then((resp) => this.buildResponseWithCommitToken(resp));
    } catch (err) {
      return Response.json(
        { error: err.message, stack: err.stack },
        { status: 500 }
      );
    }
  },

  async buildResponseWithCommitToken(resp) {
    let newToken = `token-${(++this.commitTokenNum).toLocaleString('en-US', {
      minimumIntegerDigits: 4,
      // no commas
      useGrouping: false,
    })}`;
    if (this.nextTokenExpected) {
      newToken = this.nextTokenExpected;
      this.nextTokenExpected = null;
    }
    this.commitTokensReturned.push(newToken);
    // Append an ever increasing commit token to the response.
    // Simulating the D1 eyeball worker.
    const newHeaders = new Headers(resp.headers);
    newHeaders.set('x-cf-d1-session-commit-token', newToken);
    return Response.json(await resp.json(), {
      status: resp.status,
      statusText: resp?.statusText,
      headers: newHeaders,
    });
  },

  async handleD1ApiTestRoutes(request) {
    const respondTokens = () =>
      Response.json(
        {
          commitTokensReceived: this.commitTokensReceived,
          commitTokensReturned: this.commitTokensReturned,
        },
        { status: 200 }
      );

    switch (new URL(request.url).pathname) {
      case '/commitTokens':
        // Special endpoints to accommodate our tests.
        return respondTokens();

      case '/commitTokens/nextToken':
        this.nextTokenExpected = new URL(request.url).searchParams.get('t');
        return respondTokens();

      case '/commitTokens/reset':
        this.commitTokensReceived = [];
        this.commitTokensReturned = [];
        this.commitTokenNum = 0;
        this.nextTokenExpected = null;
        return respondTokens();

      default:
        return Response.json({ error: 'invalid test route' }, { status: 404 });
    }
  },
};
