// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';

// Single-key getWithMetadata() always sets `cacheStatus` (a string, or null
// when the CF-Cache-Status header is absent). Bulk getWithMetadata(keys[])
// passes the backend's per-key `{ value, metadata }` objects through
// unchanged, so the field is absent on those entries.
expectTypeOf<
  KVNamespaceGetWithMetadataResult<string, unknown>['cacheStatus']
>().toEqualTypeOf<string | null | undefined>();

// A bulk entry as the runtime produces it: no `cacheStatus` key.
export const bulkEntry: KVNamespaceGetWithMetadataResult<
  string,
  { m: number }
> = { value: 'v', metadata: { m: 1 } };

export async function readWithMetadata(kv: KVNamespace) {
  const entries = await kv.getWithMetadata(['a', 'b']);
  expectTypeOf(entries).toEqualTypeOf<
    Map<string, KVNamespaceGetWithMetadataResult<string, unknown>>
  >();
  for (const [, entry] of entries) {
    expectTypeOf(entry.cacheStatus).toEqualTypeOf<string | null | undefined>();
  }

  const single = await kv.getWithMetadata('a');
  expectTypeOf(single.cacheStatus).toEqualTypeOf<string | null | undefined>();
}
