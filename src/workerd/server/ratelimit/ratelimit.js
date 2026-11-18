// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Rate limiting, behind the binding `cloudflare-internal:ratelimit-api`
// (src/cloudflare/internal/ratelimit-api.ts) over a service binding to this
// Worker:
//
//   wrapped = (moduleName = "cloudflare-internal:ratelimit-api",
//              innerBindings = [(name = "fetcher", service = "<this service>"),
//                               (name = "namespace", text = "1001"),
//                               (name = "limit", json = "100"),
//                               (name = "period", json = "60")])
//
// The binding checks its arguments, then POSTs /limit here: the namespace,
// key, limit, period and delta as JSON, answered with `{ success }`. Counts
// are per namespace, period and key, in fixed windows of `period` seconds:
// a call succeeds while the window's count is below the limit, and only a
// call that succeeds adds its delta. Bindings that share a namespace share
// counts, each against its own limit.
//
// The config that runs this module (`bundled = "/workerd/ratelimit.js"`) is
// an interface: cli/wrangler.rs and hand-written configs write it. Anything
// added to it later must be optional. It is a Worker with, for tests,
// optionally the binding:
//
//   TEST_CLOCK  JSON `true`: POST /clock?advance=<ms> moves this process's
//               clock.
//
// Counts are in this isolate's memory. A restart forgets them, and each node
// of a cluster counts on its own, so a cluster of n nodes allows up to n
// times the limit; Cloudflare counts once per location.

let skew = 0;
const clock = () => Date.now() + skew;

// By namespace and period, the current window: its number and each key's
// count. A new window replaces the last, so past counts take no memory.
const windows = new Map();

function limit({ namespace, key, limit, period, delta }) {
  const at = Math.floor(clock() / (period * 1000));
  const id = `${namespace}/${period}`;
  let window = windows.get(id);
  if (window?.at !== at) windows.set(id, (window = { at, counts: new Map() }));
  const count = window.counts.get(key) ?? 0;
  if (count >= limit) return false;
  window.counts.set(key, count + delta);
  return true;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === 'POST' && url.pathname === '/limit') {
      return Response.json({ success: limit(await request.json()) });
    }
    if (env.TEST_CLOCK && url.pathname === '/clock') {
      const advance = Number(url.searchParams.get('advance'));
      if (!Number.isFinite(advance)) return new Response(null, { status: 400 });
      skew += advance;
      return Response.json({ now: clock() });
    }
    return new Response('Not Found', { status: 404 });
  },
};
