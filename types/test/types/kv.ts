// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';

// Single-key getWithMetadata() always sets `cacheStatus` (a string, or null
// when the CF-Cache-Status header is absent). Bulk getWithMetadata(keys[])
// passes the backend's per-key `{ value, metadata }` objects through
// unchanged, so bulk entries have no `cacheStatus`.
expectTypeOf<
  KVNamespaceGetWithMetadataResult<string, unknown>['cacheStatus']
>().toEqualTypeOf<string | null>();

expectTypeOf<
  KVNamespaceGetWithMetadataBulkResult<string, { m: number }>
>().toEqualTypeOf<{ value: string | null; metadata: { m: number } | null }>();

// A bulk entry as the runtime produces it: no `cacheStatus` key.
export const bulkEntry: KVNamespaceGetWithMetadataBulkResult<
  string,
  { m: number }
> = { value: 'v', metadata: { m: 1 } };

export async function readWithMetadata(kv: KVNamespace) {
  const entries = await kv.getWithMetadata(['a', 'b']);
  expectTypeOf(entries).toEqualTypeOf<
    Map<string, KVNamespaceGetWithMetadataBulkResult<string, unknown>>
  >();
  for (const [, entry] of entries) {
    expectTypeOf(entry).not.toHaveProperty('cacheStatus');
  }

  const jsonEntries = await kv.getWithMetadata<{ n: number }, { m: number }>(
    ['a'],
    'json'
  );
  expectTypeOf(jsonEntries).toEqualTypeOf<
    Map<
      string,
      KVNamespaceGetWithMetadataBulkResult<{ n: number }, { m: number }>
    >
  >();

  const single = await kv.getWithMetadata('a');
  expectTypeOf(single.cacheStatus).toEqualTypeOf<string | null>();
}
