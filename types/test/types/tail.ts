// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';

expectTypeOf<TailStream.SpanOpen['root']>().toEqualTypeOf<true | undefined>();
const detachedOpen: TailStream.SpanOpen = { type: 'spanOpen', name: 'root', spanId: '1', root: true };
void detachedOpen;
// @ts-expect-error marker is omitted, never false
const notDetached: TailStream.SpanOpen = { type: 'spanOpen', name: 'child', spanId: '2', root: false };
void notDetached;

// The runtime only sets `attributes` on an Onset event when there is at least one attribute.
expectTypeOf<TailStream.Onset['attributes']>().toEqualTypeOf<
  TailStream.Attribute[] | undefined
>();

// The runtime only sets `id` on a ScriptVersion when the version has a non-null UUID.
expectTypeOf<TailStream.ScriptVersion['id']>().toEqualTypeOf<
  string | undefined
>();

export default {
  tailStream(event) {
    const onset = event.event;
    // @ts-expect-error attributes may be absent
    onset.attributes.length;
    expectTypeOf(onset.attributes?.length).toEqualTypeOf<number | undefined>();

    const version = onset.scriptVersion;
    if (version !== undefined) {
      // @ts-expect-error id may be absent
      version.id.length;
    }
    return undefined;
  },
} satisfies ExportedHandler;
