// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { onEvict, retryable } from 'cloudflare:durable-objects';
import { DurableObject } from 'cloudflare:workers';
import { expectTypeOf } from 'expect-type';

class RetryableDurableObject extends DurableObject {
  static {
    retryable(this.prototype.fetch, this.prototype.reset);
  }

  async fetch(_request: Request): Promise<Response> {
    return new Response();
  }

  reset(value: number): number {
    return value;
  }
}

expectTypeOf(retryable).returns.toBeVoid();
expectTypeOf<RetryableDurableObject['reset']>().toEqualTypeOf<
  (value: number) => number
>();

// @ts-expect-error retryable() accepts only functions.
retryable('reset');

declare const ns: DurableObjectNamespace;
expectTypeOf(
  ns.get(ns.idFromName('a'), { retry: { maxRetries: 2, timeoutMs: 1000 } })
).toEqualTypeOf<DurableObjectStub>();
expectTypeOf(
  ns.getByName('a', { retry: { maxRetries: 0 } })
).toEqualTypeOf<DurableObjectStub>();
// @ts-expect-error retry fields are numbers.
ns.get(ns.idFromName('a'), { retry: { maxRetries: '2' } });

class EvictableDurableObject extends DurableObject {
  async [onEvict](_info: DurableObjectEvictionInfo): Promise<void> {}
}

expectTypeOf(onEvict).toBeSymbol();
// An open set of reasons that still lists the known ones, for autocomplete.
expectTypeOf<DurableObjectEvictionInfo['reason']>().toEqualTypeOf<
  'inactive' | 'codeUpdated' | 'system' | (string & {})
>();
expectTypeOf<EvictableDurableObject[typeof onEvict]>().toEqualTypeOf<
  (info: DurableObjectEvictionInfo) => Promise<void>
>();
