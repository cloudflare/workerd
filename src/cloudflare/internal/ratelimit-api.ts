// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A rate limit over a service that counts: POST /limit with the namespace,
// key, limit, period and delta as JSON answers `{ success }`. The checks and
// their messages are those of Cloudflare's binding, as Miniflare has them;
// `limit` and `period` may also be given per call.

interface Env {
  fetcher: { fetch(url: string, init: RequestInit): Promise<Response> };
  namespace: string;
  limit: number;
  period: number;
}

const OPTIONS = ['key', 'limit', 'period', 'delta'];
const PERIODS = [10, 60];
const MAX_UINT32 = 0xffffffff;

function validate(test: boolean, message: string): asserts test {
  if (!test) throw new Error(message);
}

// `delta` as workerd converts a `uint32` argument.
function toDelta(delta: unknown): number {
  const value = +(delta as number);
  if (!Number.isFinite(value)) {
    throw new TypeError(
      'The value cannot be converted because it is not an integer.'
    );
  }
  if (value < 0) {
    throw new TypeError(
      'The value cannot be converted because it is negative and this API ' +
        'expects a positive number.'
    );
  }
  if (value > MAX_UINT32) {
    throw new TypeError(
      `Value out of range. Must be less than or equal to ${MAX_UINT32}.`
    );
  }
  const truncated = Math.trunc(value);
  if (truncated === 0) throw new RangeError('delta must be non-zero');
  return truncated;
}

class RateLimitImpl {
  readonly #env: Env;

  constructor(env: Env) {
    this.#env = env;
  }

  async limit(options: unknown): Promise<{ success: boolean }> {
    validate(
      typeof options === 'object' && options !== null,
      'invalid rate limit options'
    );
    const invalid = Object.keys(options).filter((k) => !OPTIONS.includes(k));
    validate(
      invalid.length === 0,
      `bad rate limit options: [${invalid.join(',')}]`
    );
    const {
      key = '',
      limit = this.#env.limit,
      period = this.#env.period,
      delta = 1,
    } = options as Record<string, unknown>;
    validate(typeof key === 'string', `invalid key: ${String(key)}`);
    validate(
      typeof limit === 'number',
      `limit must be a number: ${String(limit)}`
    );
    validate(
      typeof period === 'number',
      `period must be a number: ${String(period)}`
    );
    validate(PERIODS.includes(period), `unsupported period: ${period}`);
    const body = JSON.stringify({
      namespace: this.#env.namespace,
      key,
      limit,
      period,
      delta: toDelta(delta),
    });
    const res = await this.#env.fetcher.fetch('http://ratelimit/limit', {
      method: 'POST',
      body,
    });
    const { success } = (await res.json()) as { success: boolean };
    return { success };
  }
}

export default function makeBinding(env: Env): RateLimitImpl {
  return new RateLimitImpl(env);
}
