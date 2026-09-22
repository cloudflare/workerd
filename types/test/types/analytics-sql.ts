// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

declare const analytics: AnalyticsSQLBinding;

expectType<Promise<AnalyticsSQLResult>>(analytics.query({ query: "SELECT 1" }));

expectType<Promise<AnalyticsSQLResult<{ timestamp: string; count: number }>>>(
  analytics.query<{ timestamp: string; count: number }>({
    query: "SELECT timestamp, count FROM events WHERE account_id = ?",
    params: [123, null, true],
  }),
);

analytics.query({
  query: "SELECT * FROM events WHERE account_id = {account_id:UInt64}",
  params: { account_id: 123 },
});

// @ts-expect-error Analytics SQL parameters cannot contain objects.
analytics.query({ query: "SELECT ?", params: [{ invalid: true }] });

// @ts-expect-error Result rows must be records.
analytics.query<string>({ query: "SELECT 1" });
