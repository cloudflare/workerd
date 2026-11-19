// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Rate limiting: the entrypoint `RateLimit`, a rate limit's settings in its
// props:
//
//   (name = "LIMITER", service = (name = "<this service>",
//     entrypoint = "RateLimit", props = (json =
//       "{\"namespace\": \"1001\", \"limit\": 100, \"period\": 60}")))
//
// `limit({ key, limit, period, delta })` resolves to `{ success }`. The checks
// and their messages are those of Cloudflare's binding, as Miniflare has them;
// `limit` and `period` may also be given per call. Counts are per namespace,
// period and key, in fixed windows of `period` seconds: a call succeeds while
// the window's count is below the limit, and only a call that succeeds adds
// its delta. Bindings that share a namespace share counts, each against its
// own limit. Callers use JS RPC, so a Worker needs a compatibility date of
// 2024-04-03 or later, or the flag `rpc`.
//
// The config that runs this module (`bundled = "/workerd/ratelimit.js"`) is
// an interface: cli/wrangler.rs and hand-written configs write it. Anything
// added to it later must be optional. It is a Worker with, for tests,
// optionally the binding:
//
//   TEST_CLOCK  JSON `true`: POST /clock?advance=<ms> moves this process's
//               clock.
//
// Counts are in this isolate's memory, so a restart forgets them. They take
// memory for the keys of each namespace and period's current window; a new
// window frees the last.

import { WorkerEntrypoint } from 'cloudflare:workers';

const OPTIONS = ['key', 'limit', 'period', 'delta'];
const PERIODS = [10, 60];
const MAX_UINT32 = 0xffffffff;

let skew = 0;
const clock = () => Date.now() + skew;

// By namespace and period, the current window: its number and each key's
// count.
const windows = new Map();

function validate(test, message) {
  if (!test) throw new Error(message);
}

// `delta` as workerd converts a `uint32` argument.
function toDelta(delta) {
  const value = +delta;
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

export class RateLimit extends WorkerEntrypoint {
  limit(options) {
    validate(
      typeof options === 'object' && options !== null,
      'invalid rate limit options'
    );
    const invalid = Object.keys(options).filter((k) => !OPTIONS.includes(k));
    validate(
      invalid.length === 0,
      `bad rate limit options: [${invalid.join(',')}]`
    );
    const props = this.ctx.props;
    const {
      key = '',
      limit = props.limit,
      period = props.period,
      delta = 1,
    } = options;
    validate(typeof key === 'string', `invalid key: ${String(key)}`);
    validate(typeof limit === 'number', `limit must be a number: ${limit}`);
    validate(typeof period === 'number', `period must be a number: ${period}`);
    validate(PERIODS.includes(period), `unsupported period: ${period}`);
    const weight = toDelta(delta);

    const at = Math.floor(clock() / (period * 1000));
    const id = `${props.namespace}/${period}`;
    let window = windows.get(id);
    if (window?.at !== at)
      windows.set(id, (window = { at, counts: new Map() }));
    const count = window.counts.get(key) ?? 0;
    if (count >= limit) return { success: false };
    window.counts.set(key, count + weight);
    return { success: true };
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (env.TEST_CLOCK && url.pathname === '/clock') {
      const advance = Number(url.searchParams.get('advance'));
      if (!Number.isFinite(advance)) return new Response(null, { status: 400 });
      skew += advance;
      return Response.json({ now: clock() });
    }
    return new Response('Not Found', { status: 404 });
  },
};
