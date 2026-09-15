// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';
import type * as Stable from '../../definitions';

expectTypeOf<
  'Datagram' extends keyof typeof Stable ? true : false
>().toEqualTypeOf<false>();
expectTypeOf<
  'Datagram' extends keyof Stable.ServiceWorkerGlobalScope ? true : false
>().toEqualTypeOf<false>();
expectTypeOf<ConstructorParameters<typeof Datagram>>().toEqualTypeOf<
  [data: Uint8Array]
>();
expectTypeOf<Datagram['data']>().toEqualTypeOf<Uint8Array>();
