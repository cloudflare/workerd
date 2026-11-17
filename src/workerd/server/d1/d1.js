// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// D1, behind the binding `cloudflare-internal:d1-api`
// (src/cloudflare/internal/d1-api.ts) over a service binding to this Worker:
//
//   wrapped = (moduleName = "cloudflare-internal:d1-api",
//              innerBindings = [(name = "fetcher", service = (
//                name = "<this service>",
//                props = (json = "{\"database\": \"<id>\"}")))])
//
// Both protocols the binding speaks are served: the RPC method `query()`,
// used under the `d1_binding_jsrpc` compatibility flag, and POST /query and
// /execute, with one statement or an array of them as JSON.
//
// The config that runs this module (`bundled = "/workerd/d1.js"`) is an
// interface: cli/wrangler.rs and hand-written configs write it. Anything
// added to it later must be optional. It is:
//
//   - a Worker with RPC: a compatibility date of 2024-04-03 or later, or
//     the `rpc` flag;
//   - the Durable Object class `D1Database`, SQLite-backed (`enableSql`),
//     stored on a local disk. Its `uniqueKey` and that disk are where the
//     databases live: a config that changes either starts empty;
//   - the binding DATABASE, the `D1Database` class.
//
// A database is one Durable Object (`D1Database`, by `idFromName`) whose
// SQLite database is the application's, so in a cluster it replicates and
// fails over like any other object, and its throughput is one object's.
// Each call runs its statements in one transaction: a failure rolls back
// all of them. SQLite's own limits apply, not D1's. The object refuses
// what a Durable Object refuses (BEGIN, SAVEPOINT, ATTACH, ...), as D1
// does. There is one copy, the primary, which satisfies any session's
// bookmark, so a bookmark need only sort after the object's earlier ones:
// it is the time and a count, costs no write, and is answered only to a
// call that sends one, as Miniflare does. Statements that are only
// comments are skipped, as Miniflare and D1 skip them.
//
// `PRAGMA miniflare_d1_export(?,?,?)`, bound to noSchema, noData and then
// table names, answers the database as SQL statements, one row of them,
// as `wrangler d1 export --local` expects.

import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';

const BOOKMARK = 'x-cf-d1-session-commit-token';
const EXPORT = 'PRAGMA miniflare_d1_export(?,?,?);';

const isExport = (queries) =>
  queries.length === 1 &&
  queries[0].sql === EXPORT &&
  (queries[0].params?.length ?? 0) >= 2;

const hex = (n, width) => n.toString(16).padStart(width, '0');

export class D1Database extends DurableObject {
  #calls = 0;

  // Runs `queries`, `[{sql, params}]`, in one transaction. Answers
  // {results: [{columns, rows, meta}], bookmark}, or throws the first error;
  // the bookmark only if `bookmarked`.
  run(queries, bookmarked) {
    const work = queries.map((q) => [q, end(q.sql)]).filter(([, at]) => at);
    if (work.length === 0) throw new Error('No SQL statements detected.');
    const sql = this.ctx.storage.sql;
    const results = this.ctx.storage.transactionSync(() =>
      work.map(([q, at]) => statement(sql, q, at))
    );
    const bookmark = bookmarked
      ? `${hex(Date.now(), 12)}-${hex(++this.#calls, 8)}`
      : undefined;
    return { results, bookmark };
  }

  dump(noSchema, noData, ...tables) {
    const options = { noSchema: !!noSchema, noData: !!noData, tables };
    return [...dumpSql(this.ctx.storage.sql, options)];
  }
}

// Ported from workers-sdk c46d179e, packages/miniflare/src/workers/d1/
// dumpSql.ts (last changed e35c4a15), without its stats. Its notes: it
// duplicates SQLite's shell.c.in `.dump` as closely as possible, D1's
// d1-workers keeps a copy in sync by hand, and D1 assumes the export is
// never empty, hence the first statement.
function* dumpSql(db, { noSchema, noData, tables: only }) {
  yield `PRAGMA defer_foreign_keys=TRUE;`;
  // Empty means every table.
  const filter = new Set(only);
  // shell.c.in 105c2064, lines 8463-8469.
  const tables = db.exec(`
    SELECT name, type, sql
      FROM sqlite_schema AS o
    WHERE (true) AND type=='table'
      AND sql NOT NULL
    ORDER BY tbl_name='sqlite_sequence', rowid;
  `);
  for (const { name: table, sql } of [...tables]) {
    if (filter.size > 0 && !filter.has(table)) continue;
    if (table === 'sqlite_sequence') {
      if (!noSchema) yield `DELETE FROM sqlite_sequence;`;
    } else if (table.match(/^sqlite_stat./)) {
      // As SQLite's dump, before the table's contents.
      if (!noSchema) yield `ANALYZE sqlite_schema;`;
    } else if (sql.startsWith(`CREATE VIRTUAL TABLE`)) {
      throw new Error(
        `D1 Export error: cannot export databases with Virtual Tables (fts5)`
      );
    } else if (table.startsWith('_cf_') || table.startsWith('sqlite_')) {
      continue;
    } else if (!noSchema) {
      // As SQLite's dump: a quoted table name gets IF NOT EXISTS.
      yield sql.match(/CREATE TABLE ['"].*/)
        ? `CREATE TABLE IF NOT EXISTS ${sql.substring(13)};`
        : `${sql};`;
    }
    if (noData) continue;
    const columns = [...db.exec(`PRAGMA table_info=${escapeId(table)}`)];
    const names = columns.map((c) => escapeId(c.name)).join(',');
    const select = `SELECT ${names} FROM ${escapeId(table)};`;
    for (const row of db.exec(select).raw()) {
      const cells = row.map((cell) => {
        if (cell === null) return 'NULL';
        if (typeof cell === 'number') {
          if (!Number.isFinite(cell)) return cell > 0 ? '9e999' : '-9e999';
          return cell;
        }
        if (typeof cell === 'string') return quote(cell);
        const hex = [...new Uint8Array(cell)].map((b) =>
          b.toString(16).padStart(2, '0')
        );
        return `X'${hex.join('')}'`;
      });
      const values = cells.join(',');
      yield `INSERT INTO ${escapeId(table)} (${names}) VALUES(${values});`;
    }
  }
  if (noSchema) return;
  // shell.c.in 105c2064, lines 8473-8478, without its DESC, which SQLite
  // does not appear to apply.
  const rest = db.exec(
    'SELECT name, sql FROM sqlite_schema AS o WHERE (true) AND sql NOT NULL ' +
      "  AND type IN ('index', 'trigger', 'view') ORDER BY type COLLATE NOCASE"
  );
  for (const { name, sql } of rest) {
    if (filter.size > 0 && !filter.has(name)) continue;
    yield `${sql};`;
  }
}

// shell.c.in's output_quoted_escaped_string.
function quote(text) {
  let lf = false;
  let cr = false;
  let out = `'${text.replace(/'|(\n)|(\r)/g, (_, n, r) => {
    if (n) return ((lf = true), '\\n');
    if (r) return ((cr = true), '\\r');
    return "''";
  })}'`;
  if (cr) out = `replace(${out},'\\r',char(13))`;
  if (lf) out = `replace(${out},'\\n',char(10))`;
  return out;
}

const escapeId = (id) => `"${id.replace(/"/g, '""')}"`;

// The end of the last statement in `text`. SQLite refuses trailing
// comments and semicolons, and text that is only those; D1 does not.
function end(text) {
  let last = 0;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === '-' && text[i + 1] === '-') {
      i = text.indexOf('\n', i);
      if (i < 0) break;
    } else if (c === '/' && text[i + 1] === '*') {
      i = text.indexOf('*/', i + 2) + 1;
      if (i === 0) break;
    } else if (`'"\`[`.includes(c)) {
      i = text.indexOf(c === '[' ? ']' : c, i + 1);
      if (i < 0) return text.length;
      last = i + 1;
    } else if (!/[\s;]/.test(c)) {
      last = i + 1;
    }
  }
  return last;
}

// `text` up to `at`, the end of its last statement.
function statement(sql, { sql: text, params }, at) {
  const size = sql.databaseSize;
  const [total, lastRowId] = counters(sql);
  const values = (params ?? []).map((p) =>
    Array.isArray(p) ? new Uint8Array(p).buffer : p
  );
  const cursor = sql.exec(text.slice(0, at), ...values);
  const rows = Array.from(cursor.raw(), (row) =>
    row.map((v) => (v instanceof ArrayBuffer ? [...new Uint8Array(v)] : v))
  );
  // As Miniflare counts: rows that triggers and cascades change count too.
  const [total_, lastRowId_] = counters(sql);
  const sizeAfter = sql.databaseSize;
  return {
    columns: cursor.columnNames,
    rows,
    meta: {
      served_by: 'workerd',
      served_by_primary: true,
      total_attempts: 1,
      // workerd's clock stands still while JavaScript runs.
      timings: { sql_duration_ms: 0 },
      duration: 0,
      changes: total_ - total,
      last_row_id: lastRowId_,
      changed_db:
        total_ !== total || lastRowId_ !== lastRowId || sizeAfter !== size,
      size_after: sizeAfter,
      rows_read: cursor.rowsRead,
      rows_written: cursor.rowsWritten,
    },
  };
}

const counters = (sql) =>
  sql.exec('SELECT total_changes(), last_insert_rowid()').raw().next().value;

const objects = (columns, rows) =>
  rows.map((row) => Object.fromEntries(columns.map((c, i) => [c, row[i]])));

export default class extends WorkerEntrypoint {
  #database() {
    const id = this.ctx.props?.database;
    if (typeof id !== 'string') {
      throw new Error('No database: props must be {"database": "<id>"}.');
    }
    return this.env.DATABASE.get(this.env.DATABASE.idFromName(id));
  }

  async query({ queries, bookmark: sent }) {
    const db = this.#database();
    if (isExport(queries)) {
      const dump = await db.dump(...queries[0].params);
      const columns = dump.map((_, i) => String(i));
      return {
        results: [{ meta: {}, data: { kind: 'raw', columns, rows: [dump] } }],
      };
    }
    const { results, bookmark } = await db.run(queries, !!sent?.trim());
    return {
      results: results.map(({ columns, rows, meta }) => ({
        meta,
        data: { kind: 'raw', columns, rows },
      })),
      ...(bookmark && { bookmark }),
    };
  }

  async fetch(request) {
    const url = new URL(request.url);
    const path = url.pathname;
    if (
      request.method !== 'POST' ||
      (path !== '/query' && path !== '/execute')
    ) {
      return Response.json({ error: 'Not found' }, { status: 404 });
    }
    const rowsAndColumns =
      url.searchParams.get('resultsFormat') === 'ROWS_AND_COLUMNS';
    try {
      const body = await request.json();
      const queries = Array.isArray(body) ? body : [body];
      if (isExport(queries)) {
        const dump = await this.#database().dump(...queries[0].params);
        return Response.json([{ success: true, results: [dump], meta: {} }]);
      }
      const { results, bookmark } = await this.#database().run(
        queries,
        !!request.headers.get(BOOKMARK)?.trim()
      );
      const answers = results.map(({ columns, rows, meta }) => ({
        success: true,
        results: rowsAndColumns ? { columns, rows } : objects(columns, rows),
        meta,
      }));
      return Response.json(answers, {
        headers: bookmark ? { [BOOKMARK]: bookmark } : {},
      });
    } catch (e) {
      return Response.json({ success: false, error: String(e.message ?? e) });
    }
  }
}
