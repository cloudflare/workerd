interface D1Meta {
  duration: number;
  size_after: number;
  rows_read: number;
  rows_written: number;
  last_row_id: number;
  changed_db: boolean;
  changes: number;
}

interface D1Response {
  success: true;
  meta: D1Meta & Record<string, unknown>;
  error?: never;
}

type D1Result<T = unknown> = D1Response & {
  results: T[];
};

interface D1ExecResult {
  count: number;
  duration: number;
}

type D1SessionConstraint =
  // Indicates that the first query should go to the primary, and the rest queries
  // using the same D1DatabaseSession will go to any replica that is consistent with
  // the bookmark maintained by the session (returned by the first query).
  | "first-primary"
  // Indicates that the first query can go anywhere (primary or replica), and the rest queries
  // using the same D1DatabaseSession will go to any replica that is consistent with
  // the bookmark maintained by the session (returned by the first query).
  | "first-unconstrained";
type D1SessionBookmark = string;

declare abstract class D1Database {
  prepare(query: string): D1PreparedStatement;
  batch<T = unknown>(statements: D1PreparedStatement[]): Promise<D1Result<T>[]>;
  exec(query: string): Promise<D1ExecResult>;

  /**
   * Creates a new D1 Session anchored at the given constraint or the bookmark.
   * All queries executed using the created session will have sequential consistency,
   * meaning that all writes done through the session will be visible in subsequent reads.
   *
   * @param constraintOrBookmark Either the session constraint or the explicit bookmark to anchor the created session.
   */
  withSession(
    constraintOrBookmark:
      | D1SessionBookmark
      | D1SessionConstraint
      | null
      | undefined
  ): D1DatabaseSession;

  /**
   * @deprecated dump() will be removed soon, only applies to deprecated alpha v1 databases.
   */
  dump(): Promise<ArrayBuffer>;
}

declare abstract class D1DatabaseSession {
  prepare(query: string): D1PreparedStatement;
  batch<T = unknown>(statements: D1PreparedStatement[]): Promise<D1Result<T>[]>;

  /**
   * @returns The latest session bookmark across all executed queries on the session.
   *          If no query has been executed yet, `null` is returned.
   */
  getBookmark(): D1SessionBookmark | null;
}

declare abstract class D1PreparedStatement {
  bind(...values: unknown[]): D1PreparedStatement;
  first<T = unknown>(colName: string): Promise<T | null>;
  first<T = Record<string, unknown>>(): Promise<T | null>;
  run<T = Record<string, unknown>>(): Promise<D1Result<T>>;
  all<T = Record<string, unknown>>(): Promise<D1Result<T>>;
  raw<T = unknown[]>(options: {
    columnNames: true;
  }): Promise<[string[], ...T[]]>;
  raw<T = unknown[]>(options?: { columnNames?: false }): Promise<T[]>;
}
