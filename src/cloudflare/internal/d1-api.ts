type DatabaseBinding = {
    fetch: typeof fetch;
};

export type D1Result<T = unknown> = {
    results?: T[];
    success: boolean;
    error?: string;
    meta: any;
};

export type D1ExecResult = {
    count: number;
    duration: number;
};

type SQLError = {
    error: string;
};

export class D1Database {
    private readonly binding: DatabaseBinding;

    constructor(binding: DatabaseBinding) {
        this.binding = binding;
    }

    prepare(query: string): D1PreparedStatement {
        return new D1PreparedStatement(this, query);
    }

    async dump(): Promise<ArrayBuffer> {
        const response = await this.binding.fetch("http://d1/dump", {
            method: "POST",
            headers: {
                "content-type": "application/json",
            },
        });
        if (response.status !== 200) {
            try {
                const err = (await response.json()) as SQLError;
                throw new Error(`D1_DUMP_ERROR: ${err.error}`, {
                    cause: new Error(err.error),
                });
            } catch (e) {
                throw new Error(`D1_DUMP_ERROR: Status + ${response.status}`, {
                    cause: new Error("Status " + response.status),
                });
            }
        }
        return await response.arrayBuffer();
    }

    async batch<T = unknown>(
        statements: D1PreparedStatement[]
    ): Promise<D1Result<T>[]> {
        const exec = await this._send(
            "/query",
            statements.map((s: D1PreparedStatement) => s.statement),
            statements.map((s: D1PreparedStatement) => s.params)
        );
        return exec as D1Result<T>[];
    }

    async exec<T = unknown>(query: string): Promise<D1ExecResult> {
        // should be /execute - see CFSQL-52
        const lines = query.trim().split("\n");
        const _exec = await this._send<T>("/query", lines, [], false);
        const exec = Array.isArray(_exec) ? _exec : [_exec];
        const error = exec
            .map((r) => {
                return r.error ? 1 : 0;
            })
            .indexOf(1);
        if (error !== -1) {
            throw new Error(`D1_EXEC_ERROR: Error in line ${error + 1}: ${lines[error]}: ${exec[error].error}`, {
                cause: new Error(
                    "Error in line " +
                        (error + 1) +
                        ": " +
                        lines[error] +
                        ": " +
                        exec[error].error
                ),
            });
        } else {
            return {
                count: exec.length,
                duration: exec.reduce((p, c) => {
                    return p + c.meta.duration;
                }, 0),
            };
        }
    }

    async _send<T = unknown>(
        endpoint: string,
        query: any,
        params: any[],
        dothrow: boolean = true
    ): Promise<D1Result<T>[] | D1Result<T>> {
        /* this needs work - we currently only support ordered ?n params */
        const body = JSON.stringify(
            typeof query == "object"
                ? (query as any[]).map((s: string, index: number) => {
                      return { sql: s, params: params[index] };
                  })
                : {
                      sql: query,
                      params: params,
                  }
        );

        const response = await this.binding.fetch(
            new URL(endpoint, "http://d1"),
            {
                method: "POST",
                headers: {
                    "content-type": "application/json",
                },
                body,
            }
        );

        try {
            const answer = await response.json();

            if ((answer as any).error && dothrow) {
                const err = answer as SQLError;
                throw new Error(`D1_ERROR: ${err.error}`, { cause: new Error(err.error) });
            } else {
                return Array.isArray(answer)
                    ? (answer.map((r) => mapD1Result(r)) as D1Result<T>[])
                    : (mapD1Result(answer) as D1Result<T>);
            }
        } catch (e) {
            throw new Error(`D1_ERROR: ${e.cause || "Something went wrong"}`, {
                cause: new Error(e.cause || "Something went wrong"),
            });
        }
    }
}

export class D1PreparedStatement {
    readonly statement: string;
    private readonly database: D1Database;
    params: any[];

    constructor(database: D1Database, statement: string, values?: any) {
        this.database = database;
        this.statement = statement;
        this.params = values || [];
    }

    bind(...values: any[]) {
        // Validate value types
        for (var r in values) {
            switch (typeof values[r]) {
                case "number":
                case "string":
                    break;
                case "object":
                    // nulls are objects in javascript
                    if (values[r] == null) break;
                    // arrays with uint8's are good
                    if (
                        Array.isArray(values[r]) &&
                        values[r]
                            .map((b: any) => {
                                return typeof b == "number" && b >= 0 && b < 256
                                    ? 1
                                    : 0;
                            })
                            .indexOf(0) == -1
                    )
                        break;
                    // convert ArrayBuffer to array
                    if (values[r] instanceof ArrayBuffer) {
                        values[r] = Array.from(new Uint8Array(values[r]));
                        break;
                    }
                    // convert view to array
                    if (ArrayBuffer.isView(values[r])) {
                        values[r] = Array.from(values[r]);
                        break;
                    }
                default:
                    throw new Error(`D1_TYPE_ERROR: Type '${typeof values[r]}' not supported for value '${values[r]}'`, {
                        cause: new Error(
                            "Type '" +
                                typeof values[r] +
                                "' not supported for value '" +
                                values[r] +
                                "'"
                        ),
                    });
            }
        }
        return new D1PreparedStatement(this.database, this.statement, values);
    }

    async first<T = unknown>(colName?: string): Promise<T> {
        const info = firstIfArray(
            await this.database._send<T>("/query", this.statement, this.params)
        );
        const results = info.results;
        if (colName !== undefined) {
            if (results.length > 0 && results[0][colName] === undefined) {
                throw new Error(`D1_COLUMN_NOTFOUND: Column not found (${colName})`, {
                    cause: new Error("Column not found"),
                });
            }
            return results.length < 1 ? null : results[0][colName];
        } else {
            return results.length < 1 ? null : results[0];
        }
    }

    async run<T = unknown>(): Promise<D1Result<T>> {
        return firstIfArray(
            await this.database._send<T>(
                "/execute",
                this.statement,
                this.params
            )
        );
    }

    async all<T = unknown>(): Promise<D1Result<T[]>> {
        return firstIfArray(
            await this.database._send<T[]>(
                "/query",
                this.statement,
                this.params
            )
        );
    }

    async raw<T = unknown>(): Promise<T[]> {
        const s = firstIfArray(
            await this.database._send<T>("/query", this.statement, this.params)
        );
        const raw = [];
        for (var r in s.results) {
            const entry = Object.keys(s.results[r]).map((k) => {
                return s.results[r][k];
            });
            raw.push(entry);
        }
        return raw;
    }
}

function firstIfArray<T>(results: T | T[]): T {
    return Array.isArray(results) ? results[0] : results;
}

function mapD1Result(result: any): D1Result {
    let map: D1Result = {
        results: result.results || [],
        success: result.success === undefined ? true : result.success,
        meta: result.meta || {},
    };
    result.error && (map.error = result.error);
    return map;
}
