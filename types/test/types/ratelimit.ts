// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

declare const limiter: RateLimit;

expectType<Promise<RateLimitOutcome>>(limiter.limit({ key: 'user-123' }));
expectType<Promise<RateLimitOutcome>>(
  limiter.limit({ key: 'user-123', delta: 5 })
);

// @ts-expect-error `delta` must be a number.
limiter.limit({ key: 'user-123', delta: '5' });
// @ts-expect-error `key` is required.
limiter.limit({ delta: 5 });
