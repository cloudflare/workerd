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
  async fetch(request, env, ctx) {
    try {
      const stub = env.db.get(env.db.idFromName('test'));
      return stub.fetch(request);
    } catch (err) {
      return Response.json(
        { error: err.message, stack: err.stack },
        { status: 500 }
      );
    }
  },
};
