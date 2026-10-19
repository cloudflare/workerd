// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';

declare const loader: WorkerLoader;
const generation = loader.createGeneration({ stamp: 36, marks: 12 });
expectTypeOf(
  generation.getSharedMemory('stamp')
).toEqualTypeOf<SharedArrayBuffer>();
expectTypeOf(generation.terminate()).toEqualTypeOf<Promise<void>>();
expectTypeOf(generation.whenTerminated()).toEqualTypeOf<Promise<void>>();
