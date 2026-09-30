// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { retryable } from 'cloudflare:durable-objects';
import { DurableObject } from 'cloudflare:workers';
import { expectTypeOf } from 'expect-type';

class RetryableDurableObject extends DurableObject {
  @retryable
  async fetch(_request: Request): Promise<Response> {
    return new Response();
  }

  @retryable
  reset(value: number): number {
    return value;
  }
}

expectTypeOf<RetryableDurableObject['reset']>().toEqualTypeOf<
  (value: number) => number
>();
