// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

// structuredClone() is declared in types/defines/structured-clone.d.ts rather
// than generated from the C++ method, so these hold whichever implementation
// the runtime installs.

import { expectTypeOf } from 'expect-type';

expectTypeOf(structuredClone({ a: 1 })).toEqualTypeOf<{ a: number }>();
expectTypeOf(structuredClone('s')).toEqualTypeOf<string>();

const buffer = new ArrayBuffer(8);
expectTypeOf(structuredClone(buffer, { transfer: [buffer] })).toEqualTypeOf<ArrayBuffer>();
structuredClone(1, {});
structuredClone(1, undefined);
// @ts-expect-error the value argument is required.
structuredClone();
// @ts-expect-error transfer is a list.
structuredClone(buffer, { transfer: buffer });

expectTypeOf<StructuredSerializeOptions>().toEqualTypeOf<{ transfer?: any[] }>();

// The global scope declares it too.
expectTypeOf(globalThis.structuredClone).toEqualTypeOf(structuredClone);
expectTypeOf(self.structuredClone).toEqualTypeOf(structuredClone);
expectTypeOf<ServiceWorkerGlobalScope['structuredClone']>().toEqualTypeOf<
  typeof structuredClone
>();
