// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The node worker in cluster-broken-node.wd-test answers `/own` itself, and
// `/thread` with the identity of its isolate, after keeping its thread busy
// for `spin` milliseconds if given: a node serving on several threads has an
// isolate per thread.
// `/<BINDING>/<path>` forwards `/<path>` to the bound service. This exposes
// failures from both a broken deployment service and a skipped deployment's
// absent service as JSON `{ error }` responses with status 500.
// `/BROKEN_DO/<name>` does the same for Durable Object `<name>` of the broken
// deployment's namespace, and adds the object's `id`.
let thread;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const [, binding, ...path] = url.pathname.split('/');
    if (binding === 'own') {
      return Response.json({ own: true });
    }
    if (binding === 'thread') {
      thread ??= crypto.randomUUID();
      const until = Date.now() + Number(url.searchParams.get('spin') ?? 0);
      while (Date.now() < until) {}
      return Response.json({ thread });
    }
    url.pathname = `/${path.join('/')}`;
    if (binding === 'BROKEN_DO') {
      const id = env.BROKEN_DO.idFromName(path[0]);
      try {
        return await env.BROKEN_DO.get(id).fetch(url, request);
      } catch (error) {
        const body = { error: String(error), id: id.toString() };
        return Response.json(body, { status: 500 });
      }
    }
    try {
      return await env[binding].fetch(url, request);
    } catch (error) {
      return Response.json({ error: String(error) }, { status: 500 });
    }
  },
};
