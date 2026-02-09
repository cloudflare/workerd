// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { withSpan } from 'cloudflare-internal:tracing-helpers';
import type { Span } from './tracing';

interface D1Meta {
  duration: number;
  size_after: number;
  rows_read: number;
  rows_written: number;
  last_row_id: number;
  changed_db: boolean;
  changes: number;

  /**
   * The region of the database instance that executed the query.
   */
  served_by_region?: string;

  /**
   * True if-and-only-if the database instance that executed the query was the primary.
   */
  served_by_primary?: boolean;

  timings?: {
    /**
     * The duration of the SQL query execution by the database instance. It doesn't include any network time.
     */
    sql_duration_ms: number;
  };

  /**
   * Number of total attempts to execute the query, due to automatic retries.
   * Note: All other fields in the response like `timings` only apply to the last attempt.
   */
  total_attempts?: number;
}

interface Fetcher {
  fetch: typeof fetch;
}

type D1Response = {
  success: true;
  meta: D1Meta & Record<string, unknown>;
  error?: never;
};

type D1Result<T = unknown> = D1Response & {
  results: T[];
};

type D1RawOptions = {
  columnNames?: boolean;
};

type D1UpstreamFailure = {
  results?: never;
  error: string;
  success: false;
  meta?: never;
};

type D1RowsColumns<T = unknown> = D1Response & {
  results: {
    columns: string[];
    rows: T[][];
  };
};

type D1UpstreamSuccess<T = unknown> =
  | D1Result<T>
  | D1Response
  | D1RowsColumns<T>;

type D1UpstreamResponse<T = unknown> = D1UpstreamSuccess<T> | D1UpstreamFailure;

type D1ExecResult = {
  count: number;
  duration: number;
};

type SQLError = {
  error: string;
};

type ResultsFormat = 'ARRAY_OF_OBJECTS' | 'ROWS_AND_COLUMNS' | 'NONE';

type D1SessionBookmarkOrConstraint = string;
type D1SessionBookmark = string;
// Indicates that the first query should go to the primary, and the rest queries
// using the same D1DatabaseSession will go to any replica that is consistent with
// the bookmark maintained by the session (returned by the first query).
const D1_SESSION_CONSTRAINT_FIRST_PRIMARY = 'first-primary';
// Indicates that the first query can go anywhere (primary or replica), and the rest queries
// using the same D1DatabaseSession will go to any replica that is consistent with
// the bookmark maintained by the session (returned by the first query).
const D1_SESSION_CONSTRAINT_FIRST_UNCONSTRAINED = 'first-unconstrained';

// Parsed by the D1 eyeball worker.
// This header is internal only for our D1 binding, not part of the public D1 REST API.
// Customers should not use this header otherwise their applications can break when we change this.
// TODO Rename this to `x-cf-d1-session-bookmark`, with coordination with the D1 internal API.
const D1_SESSION_COMMIT_TOKEN_HTTP_HEADER = 'x-cf-d1-session-commit-token';

class D1Database {
  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private readonly alwaysPrimarySession: D1DatabaseSessionAlwaysPrimary;
  protected readonly fetcher: Fetcher;

  constructor(fetcher: Fetcher) {
    this.fetcher = fetcher;
    this.alwaysPrimarySession = new D1DatabaseSessionAlwaysPrimary(
      this.fetcher
    );
  }

  prepare(query: string): D1PreparedStatement {
    return new D1PreparedStatement(this.alwaysPrimarySession, query);
  }

  async batch<T = unknown>(
    statements: D1PreparedStatement[]
  ): Promise<D1Result<T>[]> {
    return this.alwaysPrimarySession.batch(statements);
  }

  async exec(query: string): Promise<D1ExecResult> {
    return this.alwaysPrimarySession.exec(query);
  }

  withSession(
    constraintOrBookmark?: D1SessionBookmarkOrConstraint
  ): D1DatabaseSession {
    constraintOrBookmark = constraintOrBookmark?.trim();
    if (constraintOrBookmark == null || constraintOrBookmark === '') {
      constraintOrBookmark = D1_SESSION_CONSTRAINT_FIRST_UNCONSTRAINED;
    }
    return new D1DatabaseSession(this.fetcher, constraintOrBookmark);
  }

  /**
   * @deprecated
   */
  async dump(): Promise<ArrayBuffer> {
    return this.alwaysPrimarySession.dump();
  }
}

class D1DatabaseSession {
  protected fetcher: Fetcher;
  protected bookmarkOrConstraint: D1SessionBookmarkOrConstraint;

  constructor(
    fetcher: Fetcher,
    bookmarkOrConstraint: D1SessionBookmarkOrConstraint
  ) {
    this.fetcher = fetcher;
    this.bookmarkOrConstraint = bookmarkOrConstraint;

    if (!this.bookmarkOrConstraint) {
      throw new Error('D1_SESSION_ERROR: invalid bookmark or constraint');
    }
  }

  // Update the bookmark IFF the given newBookmark is more recent.
  // The bookmark held in the session should always be the latest value we
  // have observed in the responses to our API. There can be cases where we have concurrent
  // queries running within the same session, and therefore here we ensure we only
  // retain the latest bookmark received.
  // @returns the final bookmark after the update.
  protected _updateBookmark(
    newBookmark: D1SessionBookmark
  ): D1SessionBookmark | null {
    newBookmark = newBookmark.trim();
    if (!newBookmark) {
      // We should not be receiving invalid bookmarks, but just be defensive.
      return this.getBookmark();
    }
    const currentBookmark = this.getBookmark();
    if (
      currentBookmark === null ||
      currentBookmark.localeCompare(newBookmark) < 0
    ) {
      this.bookmarkOrConstraint = newBookmark;
    }
    return this.getBookmark();
  }

  prepare(sql: string): D1PreparedStatement {
    return new D1PreparedStatement(this, sql);
  }

  async batch<T = unknown>(
    statements: D1PreparedStatement[]
  ): Promise<D1Result<T>[]> {
    return withSpan('d1_batch', async (span) => {
      span.setAttribute('db.system.name', 'cloudflare-d1');
      span.setAttribute('db.operation.name', 'batch');
      span.setAttribute(
        'db.query.text',
        statements.map((s: D1PreparedStatement) => s.statement).join('\n')
      );
      span.setAttribute('db.operation.batch.size', statements.length);
      span.setAttribute('cloudflare.binding.type', 'D1');
      span.setAttribute(
        'cloudflare.d1.query.bookmark',
        this.getBookmark() ?? undefined
      );

      const exec = (await this._sendOrThrow(
        '/query',
        statements.map((s: D1PreparedStatement) => s.statement),
        statements.map((s: D1PreparedStatement) => s.params),
        'ROWS_AND_COLUMNS',
        span
      )) as D1UpstreamSuccess<T>[];

      span.setAttribute(
        'cloudflare.d1.response.bookmark',
        this.getBookmark() ?? undefined
      );
      addAggregatedD1MetaToSpan(
        span,
        exec.map((e) => e.meta)
      );

      return exec.map(toArrayOfObjects);
    });
  }

  // Returns the latest bookmark we received from all responses processed so far.
  // It does not return constraints that might have be passed during the session creation.
  getBookmark(): D1SessionBookmark | null {
    switch (this.bookmarkOrConstraint) {
      // First to any replica, and then anywhere that satisfies the bookmark.
      case D1_SESSION_CONSTRAINT_FIRST_UNCONSTRAINED:
        return null;
      // First to primary, and then anywhere that satisfies the bookmark.
      case D1_SESSION_CONSTRAINT_FIRST_PRIMARY:
        return null;
      default:
        return this.bookmarkOrConstraint;
    }
  }

  // fetch will append the bookmark header to all outgoing fetch calls.
  // The response headers are parsed automatically, extracting the bookmark
  // from the response headers and updating it through `_updateBookmark(token)`.
  protected async _wrappedFetch(
    input: RequestInfo | URL,
    init?: RequestInit
  ): Promise<Response> {
    const h = new Headers(init?.headers);

    // We send either a constraint, or a bookmark, and the eyeball worker will figure out
    // what to do based on the value. This simulates the same flow as the REST API would behave too.
    if (this.bookmarkOrConstraint) {
      h.set(D1_SESSION_COMMIT_TOKEN_HTTP_HEADER, this.bookmarkOrConstraint);
    }

    if (!init) {
      init = { headers: h };
    } else {
      init.headers = h;
    }
    return this.fetcher.fetch(input, init).then((resp) => {
      const newBookmark = resp.headers.get(D1_SESSION_COMMIT_TOKEN_HTTP_HEADER);
      if (newBookmark) {
        this._updateBookmark(newBookmark);
      }
      return resp;
    });
  }

  async _sendOrThrow<T = unknown>(
    endpoint: string,
    query: string | string[],
    params: unknown[],
    resultsFormat: ResultsFormat,
    span: Span
  ): Promise<D1UpstreamSuccess<T>[] | D1UpstreamSuccess<T>> {
    const results = await this._send(
      endpoint,
      query,
      params,
      resultsFormat,
      span
    );
    const firstResult = firstIfArray(results);
    if (!firstResult.success) {
      span.setAttribute('error.type', firstResult.error);
      throw new Error(`D1_ERROR: ${firstResult.error}`, {
        cause: new Error(firstResult.error),
      });
    } else {
      return results as D1UpstreamSuccess<T>[] | D1UpstreamSuccess<T>;
    }
  }

  async _send<T = unknown>(
    endpoint: string,
    query: string | string[],
    params: unknown[],
    resultsFormat: ResultsFormat,
    span: Span
  ): Promise<D1UpstreamResponse<T>[] | D1UpstreamResponse<T>> {
    /* this needs work - we currently only support ordered ?n params */
    const body = JSON.stringify(
      Array.isArray(query)
        ? query.map((s: string, index: number) => {
            return { sql: s, params: params[index] };
          })
        : {
            sql: query,
            params: params,
          }
    );

    const url = new URL(endpoint, 'http://d1');
    url.searchParams.set('resultsFormat', resultsFormat);
    const response = await this._wrappedFetch(url.href, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
      },
      body,
    });

    try {
      const answer = await toJson<
        D1UpstreamResponse<T>[] | D1UpstreamResponse<T>
      >(response);

      if (Array.isArray(answer)) {
        return answer.map((r: D1UpstreamResponse<T>) => mapD1Result<T>(r));
      } else {
        return mapD1Result<T>(answer);
      }
    } catch (_e: unknown) {
      const e = _e as Error;
      const message =
        (e.cause as Error | undefined)?.message ||
        e.message ||
        'Something went wrong';
      span.setAttribute('error.type', message);
      throw new Error(`D1_ERROR: ${message}`, {
        cause: new Error(message),
      });
    }
  }
}

class D1DatabaseSessionAlwaysPrimary extends D1DatabaseSession {
  constructor(fetcher: Fetcher) {
    // Will always go to primary, since we won't be ever updating this constraint.
    super(fetcher, D1_SESSION_CONSTRAINT_FIRST_PRIMARY);
  }

  // We ignore bookmarks for this special type of session,
  // since all queries are sent to the primary.
  override _updateBookmark(
    _newBookmark: D1SessionBookmark
  ): D1SessionBookmark | null {
    return null;
  }

  // There is no bookmark returned ever by this special type of session,
  // since all queries are sent to the primary.
  override getBookmark(): D1SessionBookmark | null {
    return null;
  }

  //////////////////////////////////////////////////////////////////////////////////////////////
  // These are only used by the D1Database which is our existing API pre-Sessions API.
  // For backwards compatibility they always go to the primary database.
  //

  async exec(query: string): Promise<D1ExecResult> {
    return withSpan('d1_exec', async (span) => {
      span.setAttribute('db.system.name', 'cloudflare-d1');
      span.setAttribute('db.operation.name', 'exec');
      span.setAttribute('db.query.text', query);
      span.setAttribute('cloudflare.binding.type', 'D1');

      // TODO: splitting by lines is overly simplification because a single line
      // can contain multiple statements (ex: `select 1; select 2;`).
      // Also, a statement can span multiple lines...
      // Either, we should do a more reasonable job to split the query into multiple statements
      // like we do in the D1 codebase or we report a simpler error without the line number.
      const lines = query.trim().split('\n');
      const _exec = await this._send('/execute', lines, [], 'NONE', span);
      const exec = Array.isArray(_exec) ? _exec : [_exec];

      let duration = 0;
      const metas: D1Meta[] = [];
      for (let i = 0; i < exec.length; i++) {
        const res = exec[i];
        if (!res?.success) {
          span.setAttribute('error.type', `Error in line ${i + 1}`);
          throw new Error(
            `D1_EXEC_ERROR: Error in line ${i + 1}: ${lines[i]}${res?.error ? `: ${res.error}` : ''}`,
            {
              cause: new Error(
                `Error in line ${i + 1}: ${lines[i]}${res?.error ? `: ${res.error}` : ''}`
              ),
            }
          );
        }

        duration += res.meta.duration;
        metas.push(res.meta);
      }

      if (metas.length) {
        addAggregatedD1MetaToSpan(span, metas);
      }
      return {
        count: exec.length,
        duration,
      };
    });
  }

  /**
   * DEPRECATED, TO BE REMOVED WITH NEXT BREAKING CHANGE
   * Only applies to the deprecated v1 alpha databases.
   */
  async dump(): Promise<ArrayBuffer> {
    const response = await this._wrappedFetch('http://d1/dump', {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
      },
    });
    if (response.status !== 200) {
      try {
        const err = (await response.json()) as SQLError;
        throw new Error(`D1_DUMP_ERROR: ${err.error}`, {
          cause: new Error(err.error),
        });
      } catch {
        throw new Error(`D1_DUMP_ERROR: Status + ${response.status}`, {
          cause: new Error(`Status ${response.status}`),
        });
      }
    }
    return await response.arrayBuffer();
  }
}

class D1PreparedStatement {
  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private readonly dbSession: D1DatabaseSession;
  readonly statement: string;
  readonly params: unknown[];

  constructor(
    dbSession: D1DatabaseSession,
    statement: string,
    values?: unknown[]
  ) {
    this.dbSession = dbSession;
    this.statement = statement;
    this.params = values || [];
  }

  bind(...values: unknown[]): D1PreparedStatement {
    // Validate value types
    const transformedValues = values.map((r: unknown): unknown => {
      const rType = typeof r;
      if (rType === 'number' || rType === 'string') {
        return r;
      } else if (rType === 'boolean') {
        return r ? 1 : 0;
      } else if (rType === 'object') {
        // nulls are objects in javascript
        if (r == null) return r;
        // arrays with uint8's are good
        if (
          Array.isArray(r) &&
          r.every((b: unknown) => {
            return typeof b == 'number' && b >= 0 && b < 256;
          })
        )
          return r as unknown[];
        // convert ArrayBuffer to array
        if (r instanceof ArrayBuffer) {
          return Array.from(new Uint8Array(r));
        }
        // convert view to array
        if (ArrayBuffer.isView(r)) {
          // For some reason TS doesn't think this is valid, but it is!
          return Array.from(r as unknown as ArrayLike<unknown>);
        }
      }

      throw new Error(
        `D1_TYPE_ERROR: Type '${rType}' not supported for value '${r}'`,
        {
          cause: new Error(`Type '${rType}' not supported for value '${r}'`),
        }
      );
    });
    return new D1PreparedStatement(
      this.dbSession,
      this.statement,
      transformedValues
    );
  }

  async first<T = unknown>(colName: string): Promise<T | null>;
  async first<T = Record<string, unknown>>(): Promise<T | null>;
  async first<T = unknown>(
    colName?: string
  ): Promise<Record<string, T> | T | null> {
    return withSpan('d1_first', async (span) => {
      span.setAttribute('db.system.name', 'cloudflare-d1');
      span.setAttribute('db.operation.name', 'first');
      span.setAttribute('db.query.text', this.statement);
      span.setAttribute('cloudflare.binding.type', 'D1');
      span.setAttribute(
        'cloudflare.d1.query.bookmark',
        this.dbSession.getBookmark() ?? undefined
      );

      const info = firstIfArray(
        await this.dbSession._sendOrThrow<Record<string, T>>(
          '/query',
          this.statement,
          this.params,
          'ROWS_AND_COLUMNS',
          span
        )
      );

      span.setAttribute(
        'cloudflare.d1.response.bookmark',
        this.dbSession.getBookmark() ?? undefined
      );
      addD1MetaToSpan(span, info.meta);

      const results = toArrayOfObjects(info).results;
      const hasResults = results.length > 0;
      if (!hasResults) return null;

      const firstResult = results.at(0);
      if (colName !== undefined) {
        if (firstResult?.[colName] === undefined) {
          span.setAttribute('error.type', 'Column not found');
          throw new Error(`D1_COLUMN_NOTFOUND: Column not found (${colName})`, {
            cause: new Error('Column not found'),
          });
        }
        return firstResult[colName];
      } else {
        return firstResult as Record<string, T>;
      }
    });
  }

  /* eslint-disable-next-line @typescript-eslint/no-unnecessary-type-parameters */
  async run<T = Record<string, unknown>>(): Promise<D1Response> {
    return withSpan('d1_run', async (span) => {
      span.setAttribute('db.system.name', 'cloudflare-d1');
      span.setAttribute('db.operation.name', 'run');
      span.setAttribute('db.query.text', this.statement);
      span.setAttribute('cloudflare.binding.type', 'D1');
      span.setAttribute(
        'cloudflare.d1.query.bookmark',
        this.dbSession.getBookmark() ?? undefined
      );

      const result = firstIfArray(
        await this.dbSession._sendOrThrow<T>(
          '/execute',
          this.statement,
          this.params,
          'NONE',
          span
        )
      );

      span.setAttribute(
        'cloudflare.d1.response.bookmark',
        this.dbSession.getBookmark() ?? undefined
      );
      addD1MetaToSpan(span, result.meta);
      return result;
    });
  }

  async all<T = Record<string, unknown>>(): Promise<D1Result<T[]>> {
    return withSpan('d1_all', async (span) => {
      span.setAttribute('db.system.name', 'cloudflare-d1');
      span.setAttribute('db.operation.name', 'all');
      span.setAttribute('db.query.text', this.statement);
      span.setAttribute('cloudflare.binding.type', 'D1');
      span.setAttribute(
        'cloudflare.d1.query.bookmark',
        this.dbSession.getBookmark() ?? undefined
      );

      const result = firstIfArray(
        await this.dbSession._sendOrThrow<T[]>(
          '/query',
          this.statement,
          this.params,
          'ROWS_AND_COLUMNS',
          span
        )
      );

      span.setAttribute(
        'cloudflare.d1.response.bookmark',
        this.dbSession.getBookmark() ?? undefined
      );
      addD1MetaToSpan(span, result.meta);

      return toArrayOfObjects(result);
    });
  }

  async raw<T = unknown[]>(options?: D1RawOptions): Promise<T[]> {
    return withSpan('d1_all', async (span) => {
      span.setAttribute('db.system.name', 'cloudflare-d1');
      span.setAttribute('db.operation.name', 'raw');
      span.setAttribute('db.query.text', this.statement);
      span.setAttribute('cloudflare.binding.type', 'D1');
      span.setAttribute(
        'cloudflare.d1.query.bookmark',
        this.dbSession.getBookmark() ?? undefined
      );

      const s = firstIfArray(
        await this.dbSession._sendOrThrow<Record<string, unknown>>(
          '/query',
          this.statement,
          this.params,
          'ROWS_AND_COLUMNS',
          span
        )
      );

      span.setAttribute(
        'cloudflare.d1.response.bookmark',
        this.dbSession.getBookmark() ?? undefined
      );
      addD1MetaToSpan(span, s.meta);

      // If no results returned, return empty array
      if (!('results' in s)) return [];

      // If ARRAY_OF_OBJECTS returned, extract cells
      if (Array.isArray(s.results)) {
        const raw: T[] = [];
        for (const row of s.results) {
          if (options?.columnNames && raw.length === 0) {
            raw.push(Array.from(Object.keys(row)) as T);
          }
          const entry = Object.keys(row).map((k) => {
            return row[k];
          });
          raw.push(entry as T);
        }
        return raw;
      } else {
        // Otherwise, data is already in the correct format
        return [
          ...(options?.columnNames ? [s.results.columns as T] : []),
          ...(s.results.rows as T[]),
        ];
      }
    });
  }
}

function firstIfArray<T>(results: T | T[]): T {
  return Array.isArray(results) ? (results.at(0) as T) : results;
}

// This shim may be used against an older version of D1 that doesn't support
// the ROWS_AND_COLUMNS/NONE interchange format, so be permissive here
function toArrayOfObjects<T>(response: D1UpstreamSuccess<T>): D1Result<T> {
  // If 'results' is missing from upstream, add an empty array
  if (!('results' in response))
    return {
      ...response,
      results: [],
    };

  const results = response.results;
  if (Array.isArray(results)) {
    return { ...response, results };
  } else {
    const { rows, columns } = results;
    return {
      ...response,
      results: rows.map(
        (row) =>
          Object.fromEntries(row.map((cell, i) => [columns[i], cell])) as T
      ),
    };
  }
}

function mapD1Result<T>(result: D1UpstreamResponse<T>): D1UpstreamResponse<T> {
  // The rest of the app can guarantee that success is true/false, but from the API
  // we only guarantee that error is present/absent.
  return result.error
    ? {
        success: false,
        error: result.error,
      }
    : {
        success: true,
        meta: (result as D1UpstreamSuccess).meta,
        ...('results' in result ? { results: result.results } : {}),
      };
}

async function toJson<T = unknown>(response: Response): Promise<T> {
  const body = await response.text();
  try {
    return JSON.parse(body) as T;
  } catch {
    throw new Error(`Failed to parse body as JSON, got: ${body}`);
  }
}

type PartialD1Meta = Partial<D1Meta> | undefined;

function addAggregatedD1MetaToSpan(span: Span, metas: PartialD1Meta[]): void {
  if (!metas.length) {
    return;
  }
  const aggregatedMeta = aggregateD1Meta(metas);
  addD1MetaToSpan(span, aggregatedMeta);
}

function addD1MetaToSpan(span: Span, meta: D1Meta): void {
  span.setAttribute('cloudflare.d1.response.size_after', meta.size_after);
  span.setAttribute('cloudflare.d1.response.rows_read', meta.rows_read);
  span.setAttribute('cloudflare.d1.response.rows_written', meta.rows_written);
  span.setAttribute('cloudflare.d1.response.last_row_id', meta.last_row_id);
  span.setAttribute('cloudflare.d1.response.changed_db', meta.changed_db);
  span.setAttribute('cloudflare.d1.response.changes', meta.changes);
  span.setAttribute(
    'cloudflare.d1.response.served_by_region',
    meta.served_by_region
  );
  span.setAttribute(
    'cloudflare.d1.response.served_by_primary',
    meta.served_by_primary
  );
  span.setAttribute(
    'cloudflare.d1.response.sql_duration_ms',
    meta.timings?.sql_duration_ms ?? undefined
  );
  span.setAttribute(
    'cloudflare.d1.response.total_attempts',
    meta.total_attempts
  );
}

// When a query is executing multiple statements, and we receive a D1Meta
// for each statement, we need to aggregate the meta data before we annotate
// the telemetry, with different rules for each field.
function aggregateD1Meta(metas: PartialD1Meta[]): D1Meta {
  const aggregatedMeta: D1Meta = {
    duration: 0,
    size_after: 0,
    rows_read: 0,
    rows_written: 0,
    last_row_id: 0,
    changed_db: false,
    changes: 0,
  };

  for (const meta of metas) {
    if (!meta) {
      continue;
    }

    aggregatedMeta.duration += meta.duration ?? 0;
    // for size_after, we only want the last value
    aggregatedMeta.size_after = meta.size_after ?? 0;
    aggregatedMeta.rows_read += meta.rows_read ?? 0;
    aggregatedMeta.rows_written += meta.rows_written ?? 0;
    aggregatedMeta.last_row_id = meta.last_row_id ?? 0;
    if (meta.served_by_region) {
      aggregatedMeta.served_by_region = meta.served_by_region;
    }
    if (meta.served_by_primary) {
      aggregatedMeta.served_by_primary = meta.served_by_primary;
    }
    if (meta.timings?.sql_duration_ms) {
      aggregatedMeta.timings = {
        sql_duration_ms:
          (aggregatedMeta.timings?.sql_duration_ms ?? 0) +
          meta.timings.sql_duration_ms,
      };
    }
    if (meta.total_attempts) {
      aggregatedMeta.total_attempts =
        (aggregatedMeta.total_attempts ?? 0) + meta.total_attempts;
    }
    aggregatedMeta.changes += meta.changes ?? 0;
    if (meta.changed_db) {
      aggregatedMeta.changed_db = true;
    }
  }

  return aggregatedMeta;
}

export default function makeBinding(env: { fetcher: Fetcher }): D1Database {
  return new D1Database(env.fetcher);
}
