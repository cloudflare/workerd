// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';

// `data` is unwrapped from any ArrayBuffer or ArrayBufferView, like `wasm` bytes.
expectTypeOf<WorkerLoaderModule['data']>().toEqualTypeOf<
  ArrayBuffer | ArrayBufferView | undefined
>();

export const modules: WorkerLoaderModule[] = [
  { data: new ArrayBuffer(4) },
  { data: new Uint8Array([1, 2, 3, 4]) },
  { data: new TextEncoder().encode('Hello') },
  { data: new DataView(new ArrayBuffer(4)) },
];
