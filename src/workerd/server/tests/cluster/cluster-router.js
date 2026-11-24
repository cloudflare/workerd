// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The node worker in cluster-broken-node.wd-test answers `/own` itself, and
// `/thread` with the identity of its isolate: a node serving on several threads
// has an isolate per thread. A request whose `x-thread` header names another isolate
// it refuses with 409 and its own identity, before anything else is done with
// it, for the client to send it again until that isolate serves it; it does
// not pass the header on.
// `/<BINDING>/<path>` forwards `/<path>` to the bound service. This exposes
// failures from both a broken deployment service and a skipped deployment's
// absent service as JSON `{ error }` responses with status 500.
// `/BROKEN_DO/<name>` does the same for Durable Object `<name>` of the broken
// deployment's namespace, and adds the object's `id`.
let thread;

export default {
  async fetch(request, env) {
    thread ??= crypto.randomUUID();
    const wanted = request.headers.get('x-thread');
    if (wanted !== null && wanted !== thread) {
      return Response.json({ thread }, { status: 409 });
    }
    request = new Request(request);
    request.headers.delete('x-thread');
    const url = new URL(request.url);
    const [, binding, ...path] = url.pathname.split('/');
    if (binding === 'own') {
      return Response.json({ own: true });
    }
    if (binding === 'thread') {
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
