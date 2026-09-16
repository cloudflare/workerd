/** A parameter accepted by an Analytics SQL query. */
export type AnalyticsSQLParameter = string | number | boolean | null;

/** An Analytics SQL query and its optional positional or named parameters. */
export interface AnalyticsSQLQuery {
  query: string;
  params?:
    | readonly AnalyticsSQLParameter[]
    | Readonly<Record<string, AnalyticsSQLParameter>>;
}

/** Execution statistics returned by Analytics SQL. */
export interface AnalyticsSQLStatistics {
  elapsed_ms: number;
  rows_read: number;
  bytes_read: number;
}

/** The rows and execution statistics returned by an Analytics SQL query. */
export interface AnalyticsSQLResult<
  T extends Record<string, unknown> = Record<string, unknown>,
> {
  data: T[];
  rows: number;
  statistics: AnalyticsSQLStatistics;
}

/** An account-scoped Analytics SQL binding. */
export interface AnalyticsSQLBinding {
  query<T extends Record<string, unknown> = Record<string, unknown>>(
    request: AnalyticsSQLQuery,
  ): Promise<AnalyticsSQLResult<T>>;
}
