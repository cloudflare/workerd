// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

interface Fetcher {
  fetch: typeof fetch;
}

type D1Response = {
  success: true;
  meta: Record<string, unknown>;
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
  meta: Record<string, unknown>;
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
    const exec = (await this._sendOrThrow(
      '/query',
      statements.map((s: D1PreparedStatement) => s.statement),
      statements.map((s: D1PreparedStatement) => s.params),
      'ROWS_AND_COLUMNS'
    )) as D1UpstreamSuccess<T>[];
    return exec.map(toArrayOfObjects);
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
    resultsFormat: ResultsFormat
  ): Promise<D1UpstreamSuccess<T>[] | D1UpstreamSuccess<T>> {
    const results = await this._send(endpoint, query, params, resultsFormat);
    const firstResult = firstIfArray(results);
    if (!firstResult.success) {
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
    resultsFormat: ResultsFormat
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
    const lines = query.trim().split('\n');
    const _exec = await this._send('/execute', lines, [], 'NONE');
    const exec = Array.isArray(_exec) ? _exec : [_exec];
    const error = exec
      .map((r) => {
        return r.error ? 1 : 0;
      })
      .indexOf(1);
    if (error !== -1) {
      throw new Error(
        `D1_EXEC_ERROR: Error in line ${error + 1}: ${lines[error]}: ${
          exec[error]?.error
        }`,
        {
          cause: new Error(
            `Error in line ${error + 1}: ${lines[error]}: ${exec[error]?.error}`
          ),
        }
      );
    } else {
      return {
        count: exec.length,
        duration: exec.reduce((p, c) => {
          return p + (c.meta['duration'] as number);
        }, 0),
      };
    }
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
    const info = firstIfArray(
      await this.dbSession._sendOrThrow<Record<string, T>>(
        '/query',
        this.statement,
        this.params,
        'ROWS_AND_COLUMNS'
      )
    );

    const results = toArrayOfObjects(info).results;
    const hasResults = results.length > 0;
    if (!hasResults) return null;

    const firstResult = results.at(0);
    if (colName !== undefined) {
      if (firstResult?.[colName] === undefined) {
        throw new Error(`D1_COLUMN_NOTFOUND: Column not found (${colName})`, {
          cause: new Error('Column not found'),
        });
      }
      return firstResult[colName];
    } else {
      return firstResult as Record<string, T>;
    }
  }

  /* eslint-disable-next-line @typescript-eslint/no-unnecessary-type-parameters */
  async run<T = Record<string, unknown>>(): Promise<D1Response> {
    return firstIfArray(
      await this.dbSession._sendOrThrow<T>(
        '/execute',
        this.statement,
        this.params,
        'NONE'
      )
    );
  }

  async all<T = Record<string, unknown>>(): Promise<D1Result<T[]>> {
    return toArrayOfObjects(
      firstIfArray(
        await this.dbSession._sendOrThrow<T[]>(
          '/query',
          this.statement,
          this.params,
          'ROWS_AND_COLUMNS'
        )
      )
    );
  }

  async raw<T = unknown[]>(options?: D1RawOptions): Promise<T[]> {
    const s = firstIfArray(
      await this.dbSession._sendOrThrow<Record<string, unknown>>(
        '/query',
        this.statement,
        this.params,
        'ROWS_AND_COLUMNS'
      )
    );
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
        meta: result.meta,
        error: result.error,
      }
    : {
        success: true,
        meta: result.meta,
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

export default function makeBinding(env: { fetcher: Fetcher }): D1Database {
  return new D1Database(env.fetcher);
}
